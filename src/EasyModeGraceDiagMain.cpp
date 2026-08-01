#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies easy
// mode's one-note grace period, which IntroOutroDiagMain.cpp's perfect
// auto-player can't exercise (it never mistimes or skips a press). Drives
// GameSession headlessly through the first 3 Learn sections of a chart in
// easy mode, deliberately skipping presses to script misses:
//   - Learn instance 0: skip 1 note - expect the streak never drops to 0
//     (fully forgiven).
//   - Learn instance 1: skip 4 notes in a row (1 forgiven + 3 real) -
//     expect the streak DOES drop to 0 (the real misses aren't forgiven),
//     and the clip's stem actually stops after the 3rd real miss (proving
//     the forgiven miss doesn't count toward the 3-consecutive-miss
//     threshold either - if it did, the stem would stop one miss earlier).
//   - Learn instance 2: skip 1 note again - expect it's forgiven too,
//     proving the grace period refreshes every new section
//     (BeginSection resets it).
// Otherwise plays perfectly (same auto-press timing as IntroOutroDiagMain),
// including through any remaining sections after instance 2, so the chart
// reaches Complete instead of stalling.

namespace
{

const wchar_t* PhaseName(GamePhase phase)
{
    switch (phase)
    {
        case GamePhase::Idle: return L"Idle";
        case GamePhase::CountIn: return L"CountIn";
        case GamePhase::Learning: return L"Learning";
        case GamePhase::Complete: return L"Complete";
    }
    return L"?";
}

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

// Mirrors GameSession's private FindLaneNote: looks up the note whose
// phase-within-span matches absoluteStartBeat, so this diagnostic can plan
// a release without access to GameSession's internals. Kept even though
// easy mode ignores release timing - a real key-up is still simulated, to
// keep the auto-player's shape close to IntroOutroDiagMain's.
double DurationForLaneNote(const ChartClip& clip, int lane, double absoluteStartBeat)
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat, span);
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
    if (!session.LoadChart(chartPath, /*easyMode=*/true, loadError))
    {
        wprintf(L"LoadChart failed: %ls\n", loadError.c_str());
        return 1;
    }

    session.Start();

    // How many correct presses to let through before scripting misses, per
    // Learn section instance - instance 1 needs a couple of real hits first
    // so its clip actually starts playing (StartClipLoop only runs from a
    // correct press), or there'd be no "playing" state for the 3-miss stop
    // to meaningfully transition away from.
    const int kGoodHitsBeforeMisses[3] = {0, 2, 0};

    // How many consecutive onset events to skip pressing for after that,
    // per Learn section instance (0-indexed by order reached, not by chart
    // section index - background/solo sections don't count).
    const int kMissesPerInstance[3] = {1, 4, 1};
    bool streakDroppedToZero[3] = {false, false, false};

    int learnSectionInstance = -1;
    int goodHitsThisSection = 0;
    int missesScriptedThisSection = 0;
    GamePhase lastPhase = GamePhase::Idle;
    int lastSection = -2;
    int lastStreak = 0;

    StemHandle watchedInstance1Stem;
    bool watchingInstance1Stem = false;
    bool instance1StemEverPlayed = false;
    bool instance1StemStopped = false;

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        GamePhase phase = session.Phase();
        int sectionIndex = session.CurrentSectionIndex();
        PlayMode playMode = session.CurrentPlayMode();
        bool awaitingAdvance = session.IsAwaitingAdvance();
        double secondsPerBeat = 60.0 / session.Song().bpm;

        bool sectionChangedThisTick = (phase != lastPhase || sectionIndex != lastSection);

        // Same "silent streak drop" detection IntroOutroDiagMain.cpp uses:
        // a drop that ISN'T a per-section reset means RegisterMiss()
        // actually reset it - i.e. a real (non-forgiven) miss happened.
        int streak = session.CurrentStreak();
        if (streak == 0 && lastStreak > 0 && !sectionChangedThisTick && learnSectionInstance >= 0 &&
            learnSectionInstance <= 2)
        {
            streakDroppedToZero[learnSectionInstance] = true;
            printf("[t=%.2fs]   streak dropped to 0 during learn instance %d (a real, non-forgiven miss)\n",
                   session.Clock().ElapsedSeconds(), learnSectionInstance);
        }
        lastStreak = streak;

        if (sectionChangedThisTick)
        {
            lastPhase = phase;
            lastSection = sectionIndex;
            if (phase == GamePhase::Learning && playMode == PlayMode::Learn)
            {
                ++learnSectionInstance;
                goodHitsThisSection = 0;
                missesScriptedThisSection = 0;
                const ChartClip* clip = session.CurrentClip();
                printf("[t=%.2fs] learn instance %d begins (section=%d clip=%ls)\n",
                       session.Clock().ElapsedSeconds(), learnSectionInstance, sectionIndex,
                       clip ? clip->name.c_str() : L"-");
                if (learnSectionInstance == 1)
                {
                    int clipIndex = session.Song().sections[sectionIndex].clipIndex;
                    watchedInstance1Stem = session.DebugStemHandle(clipIndex);
                    watchingInstance1Stem = true;
                }
            }
        }

        // Release any lane whose planned hold has reached its end - harmless
        // in easy mode (OnRelease just clears the hold, no judgement), kept
        // so a held lane doesn't block that lane's next note forever.
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                session.ConsumeLastJudgement();
                heldByUs[lane] = false;
            }
        }

        bool judgingLive = phase == GamePhase::Learning && playMode == PlayMode::Learn && !awaitingAdvance;
        if (judgingLive)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[sectionIndex].clipIndex];
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

                lastPressedBeat[lane] = nextBeat; // handled either way - press or deliberate skip

                bool pastGoodHitsQuota = learnSectionInstance >= 0 && learnSectionInstance <= 2 &&
                                         goodHitsThisSection >= kGoodHitsBeforeMisses[learnSectionInstance];

                if (pastGoodHitsQuota && missesScriptedThisSection < kMissesPerInstance[learnSectionInstance])
                {
                    ++missesScriptedThisSection;
                    printf("[t=%.2fs]   lane %d beat=%.2f: SKIPPING press #%d/%d this instance (scripted miss)\n",
                           session.Clock().ElapsedSeconds(), lane, nextBeat, missesScriptedThisSection,
                           kMissesPerInstance[learnSectionInstance]);
                    continue; // don't press - Update()'s press-phase timeout will register the miss
                }

                session.OnPress(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                printf("[t=%.2fs]   lane %d press beat=%.2f -> %ls (streak=%d)\n", session.Clock().ElapsedSeconds(),
                       lane, nextBeat, JudgementName(result), session.CurrentStreak());
                if (result == JudgementResult::Hit && !pastGoodHitsQuota)
                {
                    ++goodHitsThisSection;
                }
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats = DurationForLaneNote(clip, lane, nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        // Only meaningful once the stem has actually been observed playing
        // at least once - otherwise "not playing" just means it hasn't
        // started yet (StartClipLoop only runs from a correct press), not
        // that it stopped.
        if (watchingInstance1Stem && learnSectionInstance == 1)
        {
            bool playingNow = engine.IsPlaying(watchedInstance1Stem);
            if (playingNow)
            {
                instance1StemEverPlayed = true;
            }
            else if (instance1StemEverPlayed && !instance1StemStopped)
            {
                instance1StemStopped = true;
                printf("[t=%.2fs]   learn instance 1's clip stem CONFIRMED STOPPED\n",
                       session.Clock().ElapsedSeconds());
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
    printf("Final phase: %ls\n", PhaseName(session.Phase()));
    printf("Instance 0 (1 skip, expect forgiven - no streak drop): streakDropped=%s %s\n",
           streakDroppedToZero[0] ? "true" : "false", streakDroppedToZero[0] ? "** MISMATCH - expected false **" : "");
    printf("Instance 1 (2 real hits, then 4 skips: 1 forgiven + 3 real, expect a streak drop AND the "
           "already-playing stem to stop): everPlayed=%s streakDropped=%s%s stemStopped=%s%s\n",
           instance1StemEverPlayed ? "true" : "false", streakDroppedToZero[1] ? "true" : "false",
           streakDroppedToZero[1] ? "" : " ** MISMATCH - expected true **", instance1StemStopped ? "true" : "false",
           instance1StemStopped ? "" : " ** MISMATCH - expected true **");
    printf("Instance 2 (1 skip, expect forgiven again - fresh grace per section): streakDropped=%s %s\n",
           streakDroppedToZero[2] ? "true" : "false", streakDroppedToZero[2] ? "** MISMATCH - expected false **" : "");

    session.Stop();
    engine.Shutdown();
    return 0;
}
