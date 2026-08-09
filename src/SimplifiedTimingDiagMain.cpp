#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies the
// still-current parts of the "always advance" timing model after learn
// sections gained the ability to repeat until locked in (see
// RepeatUntilLockedInDiagMain.cpp for that behavior specifically - this
// file no longer covers it, since a never-pressed learn section now
// repeats forever instead of ever advancing). Plays every learn section
// perfectly (so none of them need to repeat), and specifically watches:
//   - A learn section (section 0, kick Snare): locks in and advances
//     within a single loop when played well, exactly as ever - the
//     "advance only once locked in" gate is a no-op for a player who's
//     already locking in comfortably within the first loop.
//   - A break section (the first [break] in Cool Boy.chart, "break"):
//     advances strictly on its own loop_count schedule regardless of the
//     player - break was never affected by today's learn-specific change,
//     and still can't be judged/interacted with at all.

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
    bool section0PassingEver = false;
    bool section0StillPlayingAtAdvance = false;
    bool section0AdvanceObserved = false;
    int section0ClipIndex = -1;
    int breakSectionIndex = -1;
    bool breakAdvanceObserved = false;

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    for (size_t i = 0; i < session.Song().sections.size(); ++i)
    {
        if (session.Song().sections[i].kind == SectionKind::Break)
        {
            breakSectionIndex = static_cast<int>(i);
            break;
        }
    }
    printf("First break section is index %d\n", breakSectionIndex);

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
            if (lastSection == 0 && section0ClipIndex >= 0)
            {
                section0AdvanceObserved = true;
                section0StillPlayingAtAdvance = engine.IsPlaying(session.DebugStemHandle(section0ClipIndex));
                printf("[t=%.3fs] section 0 ADVANCED - clip still playing: %s\n", session.Clock().ElapsedSeconds(),
                       section0StillPlayingAtAdvance ? "true" : "false");
            }
            if (lastSection == breakSectionIndex && breakSectionIndex >= 0)
            {
                breakAdvanceObserved = true;
                printf("[t=%.3fs] break section ADVANCED\n", session.Clock().ElapsedSeconds());
            }

            lastPhase = phase;
            lastSection = sectionIndex;
            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.3fs] phase=%ls section=%d kind=%d clip=(%ls)\n", session.Clock().ElapsedSeconds(),
                   PhaseName(phase), sectionIndex, static_cast<int>(kind), clip ? clip->name.c_str() : L"-");
            if (phase == GamePhase::Learning && kind == SectionKind::Learn && sectionIndex == 0 && !section0Seen)
            {
                section0Seen = true;
                section0ClipIndex = session.Song().sections[0].clipIndex;
            }
        }

        if (session.IsPassing() && sectionIndex == 0 && !section0PassingEver)
        {
            section0PassingEver = true;
            printf("[t=%.3fs] section 0 LOCKED IN (streak=%d)\n", session.Clock().ElapsedSeconds(),
                   session.CurrentStreak());
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

        // Play every learn section perfectly, so none of them ever need to
        // repeat - this diagnostic is about the "advances promptly when
        // played well" and "break still advances on its own" cases, not
        // the repeat behavior itself.
        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn;
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
                    double durationBeats =
                        DiagTestHelpers::DurationForLaneNote(clip, lane, session.CurrentClipOriginBeat(), nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (section0AdvanceObserved && breakAdvanceObserved)
        {
            printf("Both observed advancing - stopping early.\n");
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
    printf("Section 0 (played perfectly): passing=%s%s advanced=%s%s clipStillPlayingAtAdvance=%s%s\n",
           section0PassingEver ? "true" : "false", section0PassingEver ? "" : " ** MISMATCH - expected true **",
           section0AdvanceObserved ? "true" : "false", section0AdvanceObserved ? "" : " ** MISMATCH - expected true **",
           section0StillPlayingAtAdvance ? "true" : "false",
           section0StillPlayingAtAdvance ? "" : " ** MISMATCH - expected true (locked in) **");
    printf("Break section (never touched): advanced=%s%s\n", breakAdvanceObserved ? "true" : "false",
           breakAdvanceObserved ? "" : " ** MISMATCH - expected true **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
