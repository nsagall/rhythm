#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies the
// simplified timing model - a Learn section now always advances on a fixed
// schedule known from the instant it begins, regardless of whether the
// player ever locks it in. Drives two back-to-back Learn sections against a
// tiny synthetic chart:
//   - Section 0: press nothing at all. Expect the section to still advance
//     on schedule (not hang forever waiting for a press), and the clip to
//     have stopped playing by the time it does (never proved itself, so it
//     doesn't join the arrangement).
//   - Section 1: play perfectly. Expect IsLockedIn() to flip true partway
//     through (well before the section's own scheduled advance, which
//     should be unaffected by exactly when that happens), and the clip to
//     still be playing once the section actually advances (proving it into
//     the arrangement).

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
    if (!session.LoadChart(chartPath, /*easyMode=*/false, loadError))
    {
        wprintf(L"LoadChart failed: %ls\n", loadError.c_str());
        return 1;
    }

    session.Start();

    GamePhase lastPhase = GamePhase::Idle;
    int lastSection = -2;
    bool section0Seen = false;
    bool section0StillPlayingAtAdvance = false;
    bool section0AdvanceObserved = false;
    bool section1Seen = false;
    bool section1LockedInEver = false;
    bool section1StillPlayingAtAdvance = false;
    bool section1AdvanceObserved = false;
    int section0ClipIndex = -1;
    int section1ClipIndex = -1;

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
        SectionKind kind = session.CurrentSectionKind();
        double secondsPerBeat = 60.0 / session.Song().bpm;

        bool sectionChangedThisTick = (phase != lastPhase || sectionIndex != lastSection);
        if (sectionChangedThisTick)
        {
            // A section is ending right now if we were previously sitting
            // in section 0 or 1 - check whether its clip is still playing
            // at exactly this moment of advance.
            if (lastSection == 0 && section0ClipIndex >= 0)
            {
                section0AdvanceObserved = true;
                section0StillPlayingAtAdvance = engine.IsPlaying(session.DebugStemHandle(section0ClipIndex));
                printf("[t=%.3fs] section 0 ADVANCED - clip still playing: %s\n", session.Clock().ElapsedSeconds(),
                       section0StillPlayingAtAdvance ? "true" : "false");
            }
            if (lastSection == 1 && section1ClipIndex >= 0)
            {
                section1AdvanceObserved = true;
                section1StillPlayingAtAdvance = engine.IsPlaying(session.DebugStemHandle(section1ClipIndex));
                printf("[t=%.3fs] section 1 ADVANCED - clip still playing: %s\n", session.Clock().ElapsedSeconds(),
                       section1StillPlayingAtAdvance ? "true" : "false");
            }

            lastPhase = phase;
            lastSection = sectionIndex;
            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.3fs] phase=%ls section=%d clip=(%ls)\n", session.Clock().ElapsedSeconds(), PhaseName(phase),
                   sectionIndex, clip ? clip->name.c_str() : L"-");
            if (phase == GamePhase::Learning && kind == SectionKind::Learn)
            {
                if (sectionIndex == 0 && !section0Seen)
                {
                    section0Seen = true;
                    section0ClipIndex = session.Song().sections[0].clipIndex;
                }
                else if (sectionIndex == 1 && !section1Seen)
                {
                    section1Seen = true;
                    section1ClipIndex = session.Song().sections[1].clipIndex;
                }
            }
        }

        if (session.IsLockedIn() && sectionIndex == 1)
        {
            if (!section1LockedInEver)
            {
                printf("[t=%.3fs] section 1 LOCKED IN (streak=%d)\n", session.Clock().ElapsedSeconds(),
                       session.CurrentStreak());
            }
            section1LockedInEver = true;
        }

        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                session.ConsumeLastJudgement();
                heldByUs[lane] = false;
            }
        }

        // Only press notes during section 1 - section 0 is left completely
        // untouched on purpose, to prove the game doesn't hang waiting for
        // a press that never comes.
        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn && sectionIndex == 1;
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

                lastPressedBeat[lane] = nextBeat;
                session.OnPress(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats = DurationForLaneNote(clip, lane, nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (section0AdvanceObserved && section1AdvanceObserved)
        {
            printf("Both sections observed advancing - stopping early.\n");
            break;
        }

        if (GetTickCount() - startTick > 60000)
        {
            printf("TIMEOUT after 60s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Section 0 (never pressed): advanced=%s%s clipStillPlayingAtAdvance=%s%s\n",
           section0AdvanceObserved ? "true" : "false", section0AdvanceObserved ? "" : " ** MISMATCH - expected true **",
           section0StillPlayingAtAdvance ? "true" : "false",
           section0StillPlayingAtAdvance ? " ** MISMATCH - expected false (never locked in) **" : "");
    printf("Section 1 (played perfectly): lockedIn=%s%s advanced=%s%s clipStillPlayingAtAdvance=%s%s\n",
           section1LockedInEver ? "true" : "false", section1LockedInEver ? "" : " ** MISMATCH - expected true **",
           section1AdvanceObserved ? "true" : "false", section1AdvanceObserved ? "" : " ** MISMATCH - expected true **",
           section1StillPlayingAtAdvance ? "true" : "false",
           section1StillPlayingAtAdvance ? "" : " ** MISMATCH - expected true (locked in) **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
