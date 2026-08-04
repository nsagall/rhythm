#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): reproduces "fail a
// loop, then judging breaks" against a real chart (default Melodius, target
// clip "bass"). Plays every earlier learn section perfectly (to actually
// reach the target section for real, phase-aligned exactly like live play),
// then for the target section specifically: deliberately presses only every
// other note correctly during the first loop (skipping the rest entirely,
// guaranteeing no lock-in), forcing a repeat - then switches to playing
// perfectly from the second loop onward, watching closely for any
// judgement/streak/hold anomaly. Build with -DRHYTHM_DEBUG_JUDGEMENTS to
// also see every judgement GameSession itself records internally.

namespace
{

const wchar_t* JudgementName(JudgementResult result)
{
    switch (result)
    {
        case JudgementResult::Hit: return L"Hit";
        case JudgementResult::Miss: return L"Miss";
        case JudgementResult::None: return L"None";
    }
    return L"?";
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
    std::wstring chartPath = L"Content/Melodius/Melodius.chart";
    if (argc > 1)
    {
        std::string arg = argv[1];
        chartPath.assign(arg.begin(), arg.end());
    }
    std::string targetClipNameA = argc > 2 ? argv[2] : "bass";

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
    bool skipThisOne[kLaneCount] = {};
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    int lastSectionIndex = -1;
    int targetSectionIndex = -1;
    bool inTargetFirstLoop = false;
    double firstLoopAdvance = -1.0;
    int extensionsObserved = 0;
    bool switchedToPerfect = false;
    bool anomalyDetected = false;
    int missCountAfterSwitch = 0;
    int hitCountAfterSwitch = 0;
    int missCountBeforeSwitch = 0;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            const ChartClip* clip = session.CurrentClip();
            std::string name;
            if (clip)
            {
                name.assign(clip->name.begin(), clip->name.end());
            }
            printf("[t=%.3fs] section=%d clip=(%s) passing=%s\n", session.Clock().ElapsedSeconds(), sectionIndex,
                   name.c_str(), session.IsPassing() ? "true" : "false");
            lastSectionIndex = sectionIndex;
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                lastPressedBeat[lane] = -1.0;
            }

            if (clip && name == targetClipNameA && targetSectionIndex < 0)
            {
                targetSectionIndex = sectionIndex;
                inTargetFirstLoop = true;
                firstLoopAdvance = session.PendingAdvanceAtSeconds();
                printf("[t=%.3fs] ENTERED TARGET SECTION %d, first-loop advance=%.3f\n",
                       session.Clock().ElapsedSeconds(), sectionIndex, firstLoopAdvance);
            }
        }

        if (sectionIndex == targetSectionIndex && inTargetFirstLoop)
        {
            double advance = session.PendingAdvanceAtSeconds();
            if (advance > firstLoopAdvance + 1e-6)
            {
                extensionsObserved++;
                printf("[t=%.3fs] TARGET SECTION REPEATED (extension #%d): %.3f -> %.3f\n",
                       session.Clock().ElapsedSeconds(), extensionsObserved, firstLoopAdvance, advance);
                inTargetFirstLoop = false;
                switchedToPerfect = true;
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    lastPressedBeat[lane] = -1.0;
                }
                printf("[t=%.3fs] switching to perfect play from here on\n", session.Clock().ElapsedSeconds());
            }
        }

        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (sectionIndex == targetSectionIndex)
                {
                    printf("[t=%.3fs]   lane %d release -> %ls (streak=%d passing=%s)\n",
                           session.Clock().ElapsedSeconds(), lane, JudgementName(result), session.CurrentStreak(),
                           session.IsPassing() ? "true" : "false");
                    if (result == JudgementResult::Miss)
                    {
                        if (switchedToPerfect)
                        {
                            missCountAfterSwitch++;
                            anomalyDetected = true;
                            printf("[t=%.3fs]   *** UNEXPECTED MISS AFTER SWITCHING TO PERFECT PLAY ***\n",
                                   session.Clock().ElapsedSeconds());
                        }
                        else
                        {
                            missCountBeforeSwitch++;
                        }
                    }
                    else if (result == JudgementResult::Hit && switchedToPerfect)
                    {
                        hitCountAfterSwitch++;
                    }
                }
                heldByUs[lane] = false;
            }
        }

        GamePhase phase = session.Phase();
        SectionKind kind = session.CurrentSectionKind();
        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn;
        if (judgingLive)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[sectionIndex].clipIndex];
            double secondsPerBeat = 60.0 / session.Song().bpm;
            bool isTargetFirstLoop = (sectionIndex == targetSectionIndex) && inTargetFirstLoop;

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
                if (session.Clock().ElapsedSeconds() < onsetSeconds)
                {
                    continue;
                }

                lastPressedBeat[lane] = nextBeat;

                if (isTargetFirstLoop)
                {
                    // Deliberately skip every other note on this lane during
                    // the target section's first loop, guaranteeing no
                    // lock-in - alternates per lane so it's not always the
                    // exact same notes skipped.
                    skipThisOne[lane] = !skipThisOne[lane];
                    if (skipThisOne[lane])
                    {
                        continue; // never press it at all - press-phase timeout will miss it
                    }
                }

                session.OnPress(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (sectionIndex == targetSectionIndex)
                {
                    printf("[t=%.3fs]   lane %d press beat=%.4f -> %ls (streak=%d passing=%s)\n",
                           session.Clock().ElapsedSeconds(), lane, nextBeat, JudgementName(result),
                           session.CurrentStreak(), session.IsPassing() ? "true" : "false");
                    if (result == JudgementResult::Miss)
                    {
                        if (switchedToPerfect)
                        {
                            missCountAfterSwitch++;
                            anomalyDetected = true;
                            printf("[t=%.3fs]   *** UNEXPECTED MISS AFTER SWITCHING TO PERFECT PLAY ***\n",
                                   session.Clock().ElapsedSeconds());
                        }
                        else
                        {
                            missCountBeforeSwitch++;
                        }
                    }
                }
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats =
                        DurationForLaneNote(clip, session.CurrentClipOriginBeat(), lane, nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (switchedToPerfect && session.IsPassing() && sectionIndex != targetSectionIndex)
        {
            // Advanced past the target section after locking in - success,
            // stop here.
            printf("[t=%.3fs] advanced past target section after locking in - stopping.\n",
                   session.Clock().ElapsedSeconds());
            break;
        }

        if (GetTickCount() - startTick > 90000)
        {
            printf("TIMEOUT after 90s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Extensions observed (repeats forced): %d%s\n", extensionsObserved,
           extensionsObserved >= 1 ? "" : " ** MISMATCH - expected >= 1 **");
    printf("Misses before switching to perfect play (expected, from deliberate skips): %d\n", missCountBeforeSwitch);
    printf("Hits after switching to perfect play: %d\n", hitCountAfterSwitch);
    printf("Misses after switching to perfect play (should be 0): %d%s\n", missCountAfterSwitch,
           missCountAfterSwitch == 0 ? "" : " ** ANOMALY - unexpected misses despite perfect timing **");
    printf("Overall anomaly detected: %s\n", anomalyDetected ? "true ** BUG REPRODUCED **" : "false");

    session.Stop();
    engine.Shutdown();
    return 0;
}
