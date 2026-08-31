#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic: verifies TryBufferEarlyPress makes the early half of a section's first
// note's tolerance window reachable. That note's onset lands exactly at the section start, so
// IsLaneJudgeable is false for any press before it.
//
// Drives multilane_test.chart's count-in and, for every lane whose first note is at beat 0,
// presses partway through the early half of tolerance (while IsLaneJudgeable is still false) via
// TryBufferEarlyPress. fastTapLane is also released immediately (before the section begins),
// exercising the buffered-release path. holdThroughLane is held through the transition and
// released normally.
//
// Confirms: IsLaneJudgeable was false at press time for every buffered lane; TryBufferEarlyPress
// succeeded for all; holdThroughLane is judged Hit once the section begins; fastTapLane is judged
// Miss; no lane is left stuck "held".

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

    const ChartClip& clip = session.Song().Clips()[session.Song().Sections()[0].clipIndex];
    double secondsPerBeat = 60.0 / session.Song().Bpm();

    bool laneHasFreshFirstNote[c_LaneCount] = {};
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        laneHasFreshFirstNote[lane] =
            !clip.LaneNotes(lane).empty() && std::abs(clip.LaneNotes(lane).front().startBeat) < 1e-6;
    }

    int fastTapLane = -1;
    int holdThroughLane = -1;
    for (int lane = 0; lane < c_LaneCount; ++lane)
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

    bool triedBuffer[c_LaneCount] = {};
    bool wasJudgeableAtPressTime[c_LaneCount] = {};
    bool bufferSucceeded[c_LaneCount] = {};
    double bufferedOnsetBeat[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        bufferedOnsetBeat[lane] = -1.0;
    }
    bool heldByUs[c_LaneCount] = {};
    double releaseAtSeconds[c_LaneCount] = {};
    bool judgedHit[c_LaneCount] = {};
    bool judgedMiss[c_LaneCount] = {};
    bool anyStuckHeld = false;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        if (session.Phase() == GamePhase::CountIn)
        {
            for (int lane = 0; lane < c_LaneCount; ++lane)
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
                double toleranceSeconds = clip.StartToleranceMs() / 1000.0;
                // Deliberately in the early half of the tolerance window - the half only
                // TryBufferEarlyPress can reach.
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
            double durationBeats = clip.LaneNotes(holdThroughLane).front().durationBeats;
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
