#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"
#include "NoteLaneModel.h"

// Standalone diagnostic (not part of the normal build): verifies the
// DontFail hits-meter/note-glow visual changes -
//   1. A DontFail clip's own ClipInstance::passing never goes true (so its
//      notes never draw the green "passing" glow - see NoteLaneModel::
//      UpdateClipInstances's own comment).
//   2. scene.showHitsMeter stays true for the whole DontFail section,
//      passing or failing.
//   3. scene.hitsMeterProgress tracks live progress through the clip's
//      current loop while passing, freezes the instant a miss drops back
//      to failing, jumps to the live value again the instant it locks back
//      in, and resets to 0 if a whole loop elapses while still failing.
//
// Drives "A Real Good Time" for real (real wall-clock time, exactly like
// RepeatUntilLockedInDiagMain/ScoringDiagMain), playing every note
// perfectly through the intro sections up to the "Drums" clip (the first
// learn_mode=dontfail clip in that chart), then deliberately withholds
// presses at chosen points to exercise fail/recover/loop-reset.

namespace
{

// Mirrors RepeatUntilLockedInDiagMain's own "play perfectly" press/release
// bookkeeping - kept local rather than shared, since it's specific to this
// file's own section-driving loop (unlike DiagTestHelpers.h's
// DurationForLaneNote, used identically across many diagnostics).
struct PerfectPlayer
{
    bool heldByUs[c_LaneCount] = {};
    double releaseAtSeconds[c_LaneCount] = {};
    double lastPressedBeat[c_LaneCount] = {-1.0, -1.0, -1.0, -1.0};

    void Step(GameSession& session, bool lanesToPlay[c_LaneCount])
    {
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                session.ConsumeLastJudgement();
                heldByUs[lane] = false;
            }
        }

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex < 0)
        {
            return;
        }
        const ChartClip* clip = session.CurrentClip();
        if (!clip)
        {
            return;
        }
        double secondsPerBeat = 60.0 / session.Song().bpm;
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            if (!lanesToPlay[lane] || clip->laneNotes[lane].empty() || heldByUs[lane])
            {
                continue;
            }
            double nextBeat = session.NextExpectedBeatForLane(lane);
            if (std::abs(nextBeat - lastPressedBeat[lane]) <= 1e-6)
            {
                continue;
            }
            double onsetSeconds = nextBeat * secondsPerBeat;
            if (session.Clock().ElapsedSeconds() < onsetSeconds)
            {
                continue;
            }

            lastPressedBeat[lane] = nextBeat;
            session.OnPress(lane);
            JudgementResult result = session.ConsumeLastJudgement();
            if (result == JudgementResult::None || result == JudgementResult::Hit)
            {
                double durationBeats =
                    DiagTestHelpers::DurationForLaneNote(*clip, lane, session.CurrentClipOriginBeat(), nextBeat);
                releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                heldByUs[lane] = true;
            }
        }
    }
};

bool AnyVisibleNoteGlowsPassing(const NoteLaneScene& scene, const ChartClip* currentClip, bool& sawAny)
{
    for (const SceneNote& note : scene.notes)
    {
        if (!note.clip || note.clip->chartClip != currentClip)
        {
            continue;
        }
        sawAny = true;
        if (note.clip->passing)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/A Real Good Time/A Real Good Time.chart";
    if (argc > 1)
    {
        std::string arg = argv[1];
        chartPath.assign(arg.begin(), arg.end());
    }

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("AudioEngine::Initialize failed\n");
        return 1;
    }

    GameSession session(engine);
    std::wstring loadError;
    if (!session.LoadChart(chartPath, /*easyMode=*/false, loadError))
    {
        wprintf(L"LoadChart failed: %ls\n", loadError.c_str());
        return 1;
    }

    session.Start();

    NoteLaneModel model;
    PerfectPlayer player;
    bool allLanes[c_LaneCount] = {true, true, true, true};
    bool noLanes[c_LaneCount] = {false, false, false, false};

    bool reachedDontFail = false;
    bool everGlowedWhilePassingDontFail = false;
    bool everGlowedWhileFailingDontFail = false;
    bool sawAnyDontFailNote = false;

    bool firstLockInObserved = false;
    double progressAtFirstLockIn = -1.0;

    // Phase machine for the deliberate fail/recover/loop-reset exercise,
    // once inside the DontFail clip.
    enum class Phase
    {
        PlayingToLockIn,
        PlayingAfterLockIn,
        Failing,
        CheckedFreeze,
        RecoveringAfterFail,
        CheckedJump,
        FailingForFullLoop,
        CheckedLoopReset,
        Done
    };
    Phase phase = Phase::PlayingToLockIn;

    double phaseStartSeconds = -1.0;
    double frozenProgressSeen = -1.0;
    double progressJustBeforeFail = -1.0;
    double progressAfterJump = -1.0;
    double spanBeats = 0.0;
    double bpm = session.Song().bpm;

    bool freezeHeld = true;    // stays true unless we observe progress changing while failing
    bool jumpObserved = false; // progress rose noticeably the instant it re-passed
    bool loopResetObserved = false;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete && phase != Phase::Done)
    {
        session.Update();

        const ChartClip* clip = session.CurrentClip();
        bool inDontFail = clip && session.CurrentSectionKind() == SectionKind::Learn &&
                           clip->learnMode == LearnMode::DontFail;

        if (inDontFail && !reachedDontFail)
        {
            reachedDontFail = true;
            spanBeats = clip->spanBeats;
            printf("[t=%.3fs] entered DontFail clip (spanBeats=%.3f, hitsRequired=%d)\n",
                   session.Clock().ElapsedSeconds(), spanBeats, clip->hitsRequired);
        }

        NoteLaneScene scene = model.BuildScene(session);

        if (inDontFail)
        {
            bool sawAny = false;
            bool glowing = AnyVisibleNoteGlowsPassing(scene, clip, sawAny);
            if (sawAny)
            {
                sawAnyDontFailNote = true;
            }
            if (glowing)
            {
                if (session.IsPassing())
                {
                    everGlowedWhilePassingDontFail = true;
                }
                else
                {
                    everGlowedWhileFailingDontFail = true;
                }
            }
            if (!scene.showHitsMeter)
            {
                printf("[t=%.3fs] ** MISMATCH ** showHitsMeter false during DontFail section\n",
                       session.Clock().ElapsedSeconds());
            }

            if (session.IsPassing() && !firstLockInObserved)
            {
                firstLockInObserved = true;
                progressAtFirstLockIn = scene.hitsMeterProgress;
                printf("[t=%.3fs] first LOCK-IN, hitsMeterProgress=%.3f\n", session.Clock().ElapsedSeconds(),
                       scene.hitsMeterProgress);
            }
        }

        // Drive presses.
        bool* lanesToPlay = allLanes;
        if (reachedDontFail)
        {
            switch (phase)
            {
                case Phase::PlayingToLockIn:
                    lanesToPlay = allLanes;
                    if (session.IsPassing())
                    {
                        phase = Phase::PlayingAfterLockIn;
                        phaseStartSeconds = session.Clock().ElapsedSeconds();
                        printf("[t=%.3fs] -> PlayingAfterLockIn\n", session.Clock().ElapsedSeconds());
                    }
                    break;
                case Phase::PlayingAfterLockIn:
                    lanesToPlay = allLanes;
                    // Give it a bit of real live progress before cutting it off.
                    if (session.Clock().ElapsedSeconds() - phaseStartSeconds > 0.6)
                    {
                        progressJustBeforeFail = scene.hitsMeterProgress;
                        phase = Phase::Failing;
                        phaseStartSeconds = session.Clock().ElapsedSeconds();
                        printf("[t=%.3fs] -> Failing (progress just before = %.3f)\n", session.Clock().ElapsedSeconds(),
                               progressJustBeforeFail);
                    }
                    break;
                case Phase::Failing:
                    // Stop pressing entirely - every upcoming note times out as a miss.
                    lanesToPlay = noLanes;
                    if (!session.IsPassing())
                    {
                        if (frozenProgressSeen < 0.0)
                        {
                            frozenProgressSeen = scene.hitsMeterProgress;
                        }
                        else if (std::abs(scene.hitsMeterProgress - frozenProgressSeen) > 1e-6)
                        {
                            freezeHeld = false;
                        }
                        if (session.Clock().ElapsedSeconds() - phaseStartSeconds > 0.5)
                        {
                            printf("[t=%.3fs] -> CheckedFreeze (frozen=%.3f, held=%s)\n",
                                   session.Clock().ElapsedSeconds(), frozenProgressSeen, freezeHeld ? "true" : "false");
                            phase = Phase::RecoveringAfterFail;
                        }
                    }
                    break;
                case Phase::RecoveringAfterFail:
                    lanesToPlay = allLanes;
                    if (session.IsPassing())
                    {
                        progressAfterJump = scene.hitsMeterProgress;
                        jumpObserved = progressAfterJump > frozenProgressSeen + 1e-3;
                        printf("[t=%.3fs] RE-LOCKED IN, progress jumped %.3f -> %.3f (jump observed=%s)\n",
                               session.Clock().ElapsedSeconds(), frozenProgressSeen, progressAfterJump,
                               jumpObserved ? "true" : "false");
                        phase = Phase::FailingForFullLoop;
                        phaseStartSeconds = session.Clock().ElapsedSeconds();
                    }
                    break;
                case Phase::FailingForFullLoop:
                {
                    lanesToPlay = noLanes;
                    double loopSeconds = spanBeats * (60.0 / bpm);
                    if (!session.IsPassing() &&
                        session.Clock().ElapsedSeconds() - phaseStartSeconds > loopSeconds * 1.3)
                    {
                        loopResetObserved = scene.hitsMeterProgress < 0.35;
                        printf("[t=%.3fs] after a full loop of failing, hitsMeterProgress=%.3f (reset observed=%s)\n",
                               session.Clock().ElapsedSeconds(), scene.hitsMeterProgress,
                               loopResetObserved ? "true" : "false");
                        phase = Phase::Done;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        player.Step(session, lanesToPlay);

        if (GetTickCount() - startTick > 90000)
        {
            printf("TIMEOUT after 90s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Reached DontFail clip: %s%s\n", reachedDontFail ? "true" : "false", reachedDontFail ? "" : " ** MISMATCH **");
    printf("Saw at least one DontFail note in scene: %s%s\n", sawAnyDontFailNote ? "true" : "false",
           sawAnyDontFailNote ? "" : " ** MISMATCH **");
    printf("Note glow while PASSING (should be false - no green background): %s%s\n",
           everGlowedWhilePassingDontFail ? "true" : "false",
           everGlowedWhilePassingDontFail ? " ** MISMATCH - expected false **" : "");
    printf("Note glow while FAILING (should be false): %s%s\n", everGlowedWhileFailingDontFail ? "true" : "false",
           everGlowedWhileFailingDontFail ? " ** MISMATCH - expected false **" : "");
    printf("First lock-in progress (expect > 0, <= 1): %.3f%s\n", progressAtFirstLockIn,
           (progressAtFirstLockIn > 0.0 && progressAtFirstLockIn <= 1.0) ? "" : " ** MISMATCH **");
    printf("Progress froze while failing: %s%s\n", freezeHeld ? "true" : "false",
           freezeHeld ? "" : " ** MISMATCH - expected frozen **");
    printf("Progress jumped forward on re-lock-in: %s (%.3f -> %.3f)%s\n", jumpObserved ? "true" : "false",
           frozenProgressSeen, progressAfterJump, jumpObserved ? "" : " ** MISMATCH - expected a jump **");
    printf("Progress reset after a full loop of continued failing: %s%s\n", loopResetObserved ? "true" : "false",
           loopResetObserved ? "" : " ** MISMATCH - expected reset near 0 **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
