#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies
// GameSession's scoring (see GameSession::CurrentScore/RegisterHit/
// RegisterMiss/ScoreForHit) - the combo formula, the precise-vs-imprecise
// accuracy split, and the session/banked-score split. Drives section 0 of a
// real chart pressing NOTHING for the first couple of loop-boundary
// crossings, exactly like RepeatUntilLockedInDiagMain does - guaranteeing a
// run of real, self-healing timeout misses across every lane the pattern
// uses - then starts playing perfectly, with one deliberate exception: the
// first lane's first post-miss-phase note is pressed late on purpose (past
// half its start tolerance, but still within the full window) to exercise
// the imprecise-hit path. Once section 0 locks in and advances to section 1,
// keeps running (still pressing nothing) so section 1's own first note times
// out - confirming that miss resets only section 1's own (empty) session
// score, leaving section 0's already-banked total untouched.
//
// Mirrors GameSession.cpp's own kBaseHitScore/kComboBonusPerHit/
// kImprecisionToleranceFraction/kImpreciseHitScoreMultiplier constants
// (private to that file) so this can compute the same closed-form
// expectation independently, rather than importing GameSession's own
// internals.

namespace
{

constexpr int kBaseHitScore = 100;
constexpr int kComboBonusPerHit = 10;
constexpr double kImprecisionToleranceFraction = 0.5;
constexpr double kImpreciseHitScoreMultiplier = 0.5;

int ExpectedScoreForHit(int comboAfterHit, bool precise)
{
    int fullScore = kBaseHitScore + (comboAfterHit - 1) * kComboBonusPerHit;
    return precise ? fullScore : static_cast<int>(fullScore * kImpreciseHitScoreMultiplier);
}

double DurationForLaneNote(const ChartClip& clip, double originBeat, int lane, double absoluteStartBeat)
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat - originBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }
    for (const LaneNote& note : clip.laneNotes[lane])
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return note.durationBeats;
        }
    }
    return 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/Cool Boy/Cool Boy.chart";
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

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    double lastSeenAdvance = -1.0;
    int extensionsObserved = 0;
    bool startPressing = false;
    bool missPhaseSeen = false;
    int expectedCombo = 0;
    int expectedScore = 0;
    int hitsObserved = 0;
    bool formulaMismatch = false;
    bool firstHitAfterMissPhaseChecked = false;
    bool firstHitAfterMissPhaseOk = false;
    bool advancedObserved = false;
    int lastSectionIndex = -1;

    // The deliberate imprecise-press test - see the file's own header comment.
    int impreciseTestLane = -1; // resolved to the first lane with notes, once known
    bool impreciseTestPending = true;
    bool impreciseHitSeen = false;
    bool impreciseHitOk = false;

    // The cross-section banking test - see the file's own header comment.
    bool crossedToSection1 = false;
    int scoreAtSectionCross = -1;
    bool missSeenAfterCross = false;
    bool bankingPreservedAcrossSection = false;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        for (const GameSession::JudgementEvent& event : session.ConsumeJudgementEvents())
        {
            if (event.result == JudgementResult::Miss)
            {
                missPhaseSeen = true;
                expectedCombo = 0;
                if (crossedToSection1 && !missSeenAfterCross)
                {
                    missSeenAfterCross = true;
                    bankingPreservedAcrossSection = (session.CurrentScore() == scoreAtSectionCross);
                    printf("[t=%.3fs] section 1's own first note timed out - score=%d (expected %d, unchanged "
                           "from crossing)%s\n",
                           session.Clock().ElapsedSeconds(), session.CurrentScore(), scoreAtSectionCross,
                           bankingPreservedAcrossSection ? "" : " ** MISMATCH **");
                }
            }
        }

        if (missSeenAfterCross)
        {
            break; // both tests are fully resolved - nothing left to observe
        }

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            if (lastSectionIndex == 0 && sectionIndex == 1)
            {
                advancedObserved = true;
                crossedToSection1 = true;
                scoreAtSectionCross = session.CurrentScore();
                printf("[t=%.3fs] section 0 -> 1 (advanced), score=%d banked\n", session.Clock().ElapsedSeconds(),
                       scoreAtSectionCross);
            }
            lastSectionIndex = sectionIndex;
        }

        if (sectionIndex != 0)
        {
            // Section 1 is current now - deliberately pressing nothing here
            // (see the cross-section banking test above) is exactly what
            // drives its own first note to time out.
            if (GetTickCount() - startTick > 60000)
            {
                printf("TIMEOUT after 60s, aborting\n");
                break;
            }
            Sleep(5);
            continue;
        }

        double advance = session.PendingAdvanceAtSeconds();
        if (lastSeenAdvance >= 0.0 && advance > lastSeenAdvance + 1e-6)
        {
            extensionsObserved++;
            printf("[t=%.3fs] section 0 REPEATED (extension #%d)\n", session.Clock().ElapsedSeconds(),
                   extensionsObserved);
            if (extensionsObserved >= 2 && !startPressing)
            {
                startPressing = true;
                printf("[t=%.3fs] starting to play perfectly now (combo/score should be zero)\n",
                       session.Clock().ElapsedSeconds());
            }
        }
        lastSeenAdvance = advance;

        // A correct press only starts a hold (see GameSession::OnPress) - the
        // Hit judgement (and RegisterHit's score bump) is only produced at
        // OnRelease, once the release itself is also judged on time, so this
        // is the loop that has to inspect ConsumeJudgementEvents().
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                bool wasImpreciseTestPress = (lane == impreciseTestLane) && !impreciseTestPending && !impreciseHitSeen;

                session.OnRelease(lane);
                heldByUs[lane] = false;

                for (const GameSession::JudgementEvent& event : session.ConsumeJudgementEvents())
                {
                    if (event.result != JudgementResult::Hit)
                    {
                        continue;
                    }
                    ++expectedCombo;
                    expectedScore += ExpectedScoreForHit(expectedCombo, event.precise);
                    ++hitsObserved;
                    int actualScore = session.CurrentScore();
                    formulaMismatch |= (actualScore != expectedScore);

                    if (missPhaseSeen && !firstHitAfterMissPhaseChecked)
                    {
                        firstHitAfterMissPhaseChecked = true;
                        // This may be the same hit as the deliberate
                        // imprecise-press test below (both are "the first
                        // hit to resolve"), so the expectation has to allow
                        // for either accuracy outcome rather than assuming
                        // precise=true - only the combo/base-of-a-fresh-
                        // combo part is what this check actually cares about.
                        int expectedFreshComboScore = ExpectedScoreForHit(1, event.precise);
                        firstHitAfterMissPhaseOk = (expectedCombo == 1) && (actualScore == expectedFreshComboScore);
                        printf("[t=%.3fs] first hit after miss phase: combo=%d precise=%s score=%d%s\n",
                               session.Clock().ElapsedSeconds(), expectedCombo, event.precise ? "true" : "false",
                               actualScore,
                               firstHitAfterMissPhaseOk ? "" : " ** MISMATCH - expected combo=1, fresh-combo score **");
                    }

                    if (wasImpreciseTestPress)
                    {
                        impreciseHitSeen = true;
                        impreciseHitOk = !event.precise;
                        printf("[t=%.3fs] deliberately-late press judged: precise=%s combo=%d score-this-hit=%d%s\n",
                               session.Clock().ElapsedSeconds(), event.precise ? "true" : "false", expectedCombo,
                               ExpectedScoreForHit(expectedCombo, event.precise),
                               impreciseHitOk ? "" : " ** MISMATCH - expected precise=false **");
                    }
                }
            }
        }

        if (startPressing)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[0].clipIndex];
            double secondsPerBeat = 60.0 / session.Song().bpm;
            double toleranceSeconds = clip.startToleranceMs / 1000.0;

            if (impreciseTestLane == -1)
            {
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    if (!clip.laneNotes[lane].empty())
                    {
                        impreciseTestLane = lane;
                        break;
                    }
                }
            }

            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (clip.laneNotes[lane].empty() || heldByUs[lane])
                {
                    continue;
                }
                double nextBeat = session.NextExpectedBeatForLane(lane);
                if (std::abs(nextBeat - lastPressedBeat[lane]) <= 1e-6)
                {
                    continue;
                }
                double onsetSeconds = nextBeat * secondsPerBeat;
                double pressAfterSeconds = onsetSeconds;
                if (lane == impreciseTestLane && impreciseTestPending)
                {
                    // Deliberately wait past half the tolerance window
                    // (still well inside the full window) before pressing -
                    // see kImprecisionToleranceFraction in GameSession.cpp.
                    pressAfterSeconds = onsetSeconds + toleranceSeconds * 0.7;
                }
                if (session.Clock().ElapsedSeconds() < pressAfterSeconds)
                {
                    continue;
                }

                if (lane == impreciseTestLane && impreciseTestPending)
                {
                    impreciseTestPending = false;
                }

                lastPressedBeat[lane] = nextBeat;
                session.OnPress(lane);
                session.ConsumeJudgementEvents(); // a correct press produces no event yet - see above

                double durationBeats = DurationForLaneNote(clip, session.CurrentClipOriginBeat(), lane, nextBeat);
                releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                heldByUs[lane] = true;
            }
        }

        if (GetTickCount() - startTick > 60000)
        {
            printf("TIMEOUT after 60s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Miss phase observed (repeats before playing): %s%s\n", missPhaseSeen ? "true" : "false",
           missPhaseSeen ? "" : " ** MISMATCH - expected true **");
    printf("First hit after miss phase scored the fresh-combo base amount: %s%s\n",
           firstHitAfterMissPhaseOk ? "true" : "false", firstHitAfterMissPhaseOk ? "" : " ** MISMATCH **");
    printf("Hits observed: %d%s\n", hitsObserved, hitsObserved > 0 ? "" : " ** MISMATCH - expected > 0 **");
    printf("Score matched the closed-form combo/accuracy formula on every hit: %s\n",
           formulaMismatch ? "false ** MISMATCH **" : "true");
    printf("Deliberately-late press was judged imprecise (reduced score): %s%s\n",
           impreciseHitOk ? "true" : "false", impreciseHitSeen ? "" : " ** MISMATCH - never observed **");
    printf("Final score: %d (expected %d)\n", session.CurrentScore(), expectedScore);
    printf("Advanced to section 1: %s%s\n", advancedObserved ? "true" : "false",
           advancedObserved ? "" : " ** MISMATCH - expected true **");
    printf("Section 0's banked score survived section 1's own streak break: %s%s\n",
           bankingPreservedAcrossSection ? "true" : "false",
           missSeenAfterCross ? "" : " ** MISMATCH - never observed **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
