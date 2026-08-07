#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies the fix for
// "the very first note of a section is much harder to hit than every other
// note." That note's onset coincides exactly with the section's own start
// instant (see ChartTiming::FreshOnsetForAllLanes), so before this fix,
// IsLaneJudgeable(lane) was false for any press landing before that instant
// - even one well within the note's own start tolerance - silently
// discarding the early half of its window (see GameSession::
// TryBufferEarlyPress's own comment for the full story).
//
// Drives multilane_test.chart's count-in and, for every lane whose very
// first note sits at beat 0 of the pattern (so its onset lands exactly at
// the count-in's end), presses it partway through the *early* half of its
// start tolerance - deliberately while IsLaneJudgeable(lane) is still false
// - via TryBufferEarlyPress instead of OnPress, exactly as MainWindow now
// does. One such lane (fastTapLane) is released immediately too, still
// before the section begins - a real note has positive duration, so
// releasing before its own onset is always premature relative to its real
// end (onset + duration necessarily falls after the transition); this
// exercises the buffered-release path (see OnRelease) and confirms it
// produces a clean, definitive Miss instead of leaving a phantom stuck
// hold - the exact regression risk buffering a press-without-its-release
// would otherwise create. Another lane (holdThroughLane) is held through
// the transition and released normally afterward, once its real duration is
// known - the mainline case this fix targets. Confirms: (1) IsLaneJudgeable
// was genuinely false at press time for every buffered lane (so this only
// ever exercises the new early path, not a lucky race), (2)
// TryBufferEarlyPress reports success for all of them, (3) holdThroughLane
// is judged Hit once the section actually begins (proving the early half of
// its tolerance window - unreachable before this fix - now works),
// (4) fastTapLane is judged Miss (proving a genuinely too-early release
// still resolves definitively), and (5) no lane is left stuck "held"
// afterward.

int main(int argc, char** argv)
{
    std::wstring chartPath = L"test_charts/multilane_test.chart";
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

    const ChartClip& clip = session.Song().clips[session.Song().sections[0].clipIndex];
    double secondsPerBeat = 60.0 / session.Song().bpm;

    bool laneHasFreshFirstNote[kLaneCount] = {};
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        laneHasFreshFirstNote[lane] =
            !clip.laneNotes[lane].empty() && std::abs(clip.laneNotes[lane].front().startBeat) < 1e-6;
    }

    int fastTapLane = -1;
    int holdThroughLane = -1;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (!laneHasFreshFirstNote[lane])
        {
            continue;
        }
        if (fastTapLane < 0)
        {
            fastTapLane = lane;
        }
        else if (holdThroughLane < 0)
        {
            holdThroughLane = lane;
        }
    }

    if (fastTapLane < 0)
    {
        printf("No lane has a fresh first note at beat 0 in this chart - nothing to test\n");
        return 1;
    }
    printf("fastTapLane=%d holdThroughLane=%d\n", fastTapLane, holdThroughLane);

    bool triedBuffer[kLaneCount] = {};
    bool wasJudgeableAtPressTime[kLaneCount] = {};
    bool bufferSucceeded[kLaneCount] = {};
    double bufferedOnsetBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        bufferedOnsetBeat[lane] = -1.0;
    }
    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    bool judgedHit[kLaneCount] = {};
    bool judgedMiss[kLaneCount] = {};
    bool anyStuckHeld = false;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        if (session.Phase() == GamePhase::CountIn)
        {
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (!laneHasFreshFirstNote[lane] || triedBuffer[lane])
                {
                    continue;
                }
                if (lane != fastTapLane && lane != holdThroughLane)
                {
                    continue;
                }
                double onsetBeat = session.PreviewFirstOnsetBeatForLane(lane);
                if (onsetBeat < 0.0)
                {
                    continue;
                }
                double onsetSeconds = onsetBeat * secondsPerBeat;
                double toleranceSeconds = clip.startToleranceMs / 1000.0;
                // Deliberately in the *early* half of the tolerance window -
                // the exact half this bug used to make unreachable.
                double target = onsetSeconds - toleranceSeconds * 0.5;
                if (session.Clock().ElapsedSeconds() < target)
                {
                    continue;
                }

                triedBuffer[lane] = true;
                bufferedOnsetBeat[lane] = onsetBeat;
                wasJudgeableAtPressTime[lane] = session.IsLaneJudgeable(lane);
                bufferSucceeded[lane] = session.TryBufferEarlyPress(lane);
                printf("[t=%.4f] lane %d early press: phase=CountIn judgeable=%s bufferSucceeded=%s onset=%.4fs\n",
                       session.Clock().ElapsedSeconds(), lane, wasJudgeableAtPressTime[lane] ? "true" : "false",
                       bufferSucceeded[lane] ? "true" : "false", onsetSeconds);

                if (lane == fastTapLane)
                {
                    // Fast anticipatory tap: let go again immediately,
                    // still well before the section begins.
                    session.OnRelease(lane);
                    printf("[t=%.4f] lane %d buffered release sent (fast tap, still pre-transition)\n",
                           session.Clock().ElapsedSeconds(), lane);
                }
                else
                {
                    heldByUs[lane] = true; // release normally, later, once the real hold exists
                }
            }
        }

        if (heldByUs[holdThroughLane >= 0 ? holdThroughLane : 0] && holdThroughLane >= 0 &&
            session.IsLaneHeld(holdThroughLane) && releaseAtSeconds[holdThroughLane] == 0.0)
        {
            double durationBeats = clip.laneNotes[holdThroughLane].front().durationBeats;
            releaseAtSeconds[holdThroughLane] =
                (bufferedOnsetBeat[holdThroughLane] + durationBeats) * secondsPerBeat;
            printf("[t=%.4f] lane %d hold established, will release at t=%.4fs\n", session.Clock().ElapsedSeconds(),
                   holdThroughLane, releaseAtSeconds[holdThroughLane]);
        }
        if (holdThroughLane >= 0 && heldByUs[holdThroughLane] && releaseAtSeconds[holdThroughLane] > 0.0 &&
            session.Clock().ElapsedSeconds() >= releaseAtSeconds[holdThroughLane])
        {
            session.OnRelease(holdThroughLane);
            heldByUs[holdThroughLane] = false;
            printf("[t=%.4f] lane %d released normally, post-transition\n", session.Clock().ElapsedSeconds(),
                   holdThroughLane);
        }

        for (const GameSession::JudgementEvent& event : session.ConsumeJudgementEvents())
        {
            if (event.lane == fastTapLane || event.lane == holdThroughLane)
            {
                if (event.result == JudgementResult::Hit)
                {
                    judgedHit[event.lane] = true;
                    printf("[t=%.4f] lane %d judged HIT (precise=%s)\n", session.Clock().ElapsedSeconds(),
                           event.lane, event.precise ? "true" : "false");
                }
                else if (event.result == JudgementResult::Miss)
                {
                    judgedMiss[event.lane] = true;
                    printf("[t=%.4f] lane %d judged MISS\n", session.Clock().ElapsedSeconds(), event.lane);
                }
            }
        }

        if (triedBuffer[fastTapLane] && (holdThroughLane < 0 || triedBuffer[holdThroughLane]) &&
            (judgedHit[fastTapLane] || judgedMiss[fastTapLane]) &&
            (holdThroughLane < 0 || judgedHit[holdThroughLane] || judgedMiss[holdThroughLane]))
        {
            if (session.IsLaneHeld(fastTapLane) || (holdThroughLane >= 0 && session.IsLaneHeld(holdThroughLane)))
            {
                anyStuckHeld = true;
            }
            break;
        }

        if (GetTickCount() - startTick > 30000)
        {
            printf("TIMEOUT after 30s, aborting\n");
            break;
        }

        Sleep(2);
    }

    printf("\n=== RESULTS ===\n");
    printf("fastTapLane=%d (release also pre-transition, so its real end can't be reached): "
           "judgeableAtPressTime=%s (expect false), bufferSucceeded=%s (expect true), result=%s (expect Miss)\n",
           fastTapLane, wasJudgeableAtPressTime[fastTapLane] ? "true" : "false",
           bufferSucceeded[fastTapLane] ? "true" : "false", judgedMiss[fastTapLane] ? "Miss" : "not-Miss");
    if (holdThroughLane >= 0)
    {
        printf("holdThroughLane=%d: judgeableAtPressTime=%s (expect false), bufferSucceeded=%s (expect true), "
               "result=%s (expect Hit)\n",
               holdThroughLane, wasJudgeableAtPressTime[holdThroughLane] ? "true" : "false",
               bufferSucceeded[holdThroughLane] ? "true" : "false", judgedHit[holdThroughLane] ? "Hit" : "not-Hit");
    }
    printf("Any lane stuck held at the end: %s%s\n", anyStuckHeld ? "true" : "false",
           anyStuckHeld ? " ** MISMATCH - expected false **" : "");

    bool pass = !wasJudgeableAtPressTime[fastTapLane] && bufferSucceeded[fastTapLane] && judgedMiss[fastTapLane] &&
                !anyStuckHeld;
    if (holdThroughLane >= 0)
    {
        pass = pass && !wasJudgeableAtPressTime[holdThroughLane] && bufferSucceeded[holdThroughLane] &&
               judgedHit[holdThroughLane];
    }
    printf("%s\n", pass ? "PASS" : "FAIL");

    session.Stop();
    engine.Shutdown();
    return pass ? 0 : 1;
}
