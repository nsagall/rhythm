#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies
// GameSession's scoring (see GameSession::CurrentScore/RegisterHit/
// RegisterMiss). Drives section 0 of a real chart pressing NOTHING for the
// first couple of loop-boundary crossings, exactly like
// RepeatUntilLockedInDiagMain does - guaranteeing a run of real,
// self-healing timeout misses across every lane the pattern uses - then
// starts playing perfectly. Confirms: (1) the miss phase left the combo at
// zero (the very first hit afterward scores exactly the base amount, with
// no leftover bonus from before), and (2) every hit after that matches the
// closed-form combo formula exactly, hit for hit, all the way to lock-in.

namespace
{

constexpr int kBaseHitScore = 100;
constexpr int kComboBonusPerHit = 10;

int ExpectedScoreForHit(int comboAfterHit)
{
    return kBaseHitScore + (comboAfterHit - 1) * kComboBonusPerHit;
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
            }
        }

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            if (lastSectionIndex == 0 && sectionIndex == 1)
            {
                advancedObserved = true;
                printf("[t=%.3fs] section 0 -> 1 (advanced)\n", session.Clock().ElapsedSeconds());
                break;
            }
            lastSectionIndex = sectionIndex;
        }

        if (sectionIndex != 0)
        {
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
                session.OnRelease(lane);
                heldByUs[lane] = false;

                for (const GameSession::JudgementEvent& event : session.ConsumeJudgementEvents())
                {
                    if (event.result != JudgementResult::Hit)
                    {
                        continue;
                    }
                    ++expectedCombo;
                    expectedScore += ExpectedScoreForHit(expectedCombo);
                    ++hitsObserved;
                    int actualScore = session.CurrentScore();
                    formulaMismatch |= (actualScore != expectedScore);

                    if (missPhaseSeen && !firstHitAfterMissPhaseChecked)
                    {
                        firstHitAfterMissPhaseChecked = true;
                        firstHitAfterMissPhaseOk = (expectedCombo == 1) && (actualScore == kBaseHitScore);
                        printf("[t=%.3fs] first hit after miss phase: combo=%d score=%d%s\n",
                               session.Clock().ElapsedSeconds(), expectedCombo, actualScore,
                               firstHitAfterMissPhaseOk ? "" : " ** MISMATCH - expected combo=1, score=100 **");
                    }
                }
            }
        }

        if (startPressing)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[0].clipIndex];
            double secondsPerBeat = 60.0 / session.Song().bpm;
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
    printf("Score matched the closed-form combo formula on every hit: %s\n",
           formulaMismatch ? "false ** MISMATCH **" : "true");
    printf("Final score: %d (expected %d)\n", session.CurrentScore(), expectedScore);
    printf("Advanced to section 1: %s%s\n", advancedObserved ? "true" : "false",
           advancedObserved ? "" : " ** MISMATCH - expected true **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
