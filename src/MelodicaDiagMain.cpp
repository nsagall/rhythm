#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "ChartTiming.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): drives GameSession
// headlessly through a chart's Learn sections in order, auto-pressing every
// lane's notes at the right moment, and prints each Learn section's clip
// name, computed anchor beat per lane (mod the clip's own spanBeats - "how
// far into its own pattern is this anchor"), and IsAwaitingAdvance/streak
// info - written to chase a reported bug where Melodius.chart's "melodica"
// clip specifically (not melodica_2/3/4) starts audibly partway through its
// own pattern instead of at the top.

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
    std::wstring chartPath = L"Content/Melodius/Melodius.chart";
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

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    // Stop once we've seen the target clip's section fully begin and had a
    // moment to observe its StartClipLoop - no need to play the whole song.
    const wchar_t* targetClipName = argc > 2 ? [&]() {
        static std::wstring t;
        std::string arg = argv[2];
        t.assign(arg.begin(), arg.end());
        return t.c_str();
    }() : L"melodica";
    bool targetSeen = false;
    DWORD targetSeenAtTick = 0;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        GamePhase phase = session.Phase();
        int sectionIndex = session.CurrentSectionIndex();
        SectionKind kind = session.CurrentSectionKind();
        bool awaitingAdvance = session.IsAwaitingAdvance();
        double secondsPerBeat = 60.0 / session.Song().bpm;

        bool sectionChangedThisTick = (phase != lastPhase || sectionIndex != lastSection);
        if (sectionChangedThisTick)
        {
            lastPhase = phase;
            lastSection = sectionIndex;
            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.3fs] phase=%ls section=%d kind=%d clip=(%ls)\n", session.Clock().ElapsedSeconds(),
                   PhaseName(phase), sectionIndex, static_cast<int>(kind), clip ? clip->name.c_str() : L"-");
            if (clip && phase == GamePhase::Learning && kind == SectionKind::Learn)
            {
                printf("  spanBeats=%.4f\n", clip->spanBeats);
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    if (clip->laneNotes[lane].empty())
                    {
                        continue;
                    }
                    double anchor = session.NextExpectedBeatForLane(lane);
                    double phaseWithinSpan = std::fmod(anchor, clip->spanBeats);
                    if (phaseWithinSpan < 0.0)
                    {
                        phaseWithinSpan += clip->spanBeats;
                    }
                    printf("  lane %d anchor=%.4f (phase within pattern: %.4f / %.4f = %.1f%%)\n", lane, anchor,
                           phaseWithinSpan, clip->spanBeats, 100.0 * phaseWithinSpan / clip->spanBeats);
                }
                if (clip->name == targetClipName)
                {
                    targetSeen = true;
                    targetSeenAtTick = GetTickCount();
                }
            }
        }

        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (lastSection >= 0 && session.CurrentClip() &&
                    session.CurrentClip()->name == targetClipName)
                {
                    printf("[t=%.3fs]   lane %d release -> %ls (streak=%d) stemPlaying=%s stemPos=%.4fs\n",
                           session.Clock().ElapsedSeconds(), lane, JudgementName(result), session.CurrentStreak(),
                           engine.IsPlaying(session.DebugStemHandle(session.Song().sections[lastSection].clipIndex))
                               ? "true"
                               : "false",
                           engine.GetPositionSeconds(
                               session.DebugStemHandle(session.Song().sections[lastSection].clipIndex)));
                }
                heldByUs[lane] = false;
            }
        }

        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn && !awaitingAdvance;
        if (judgingLive)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[sectionIndex].clipIndex];
            bool isTarget = clip.name == targetClipName;
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
                if (isTarget)
                {
                    StemHandle h = session.DebugStemHandle(sectionIndex >= 0 ? session.Song().sections[sectionIndex].clipIndex : -1);
                    printf("[t=%.3fs]   lane %d press beat=%.4f -> %ls (streak=%d) stemPlaying=%s stemPos=%.4fs "
                           "nowElapsed=%.4fs\n",
                           session.Clock().ElapsedSeconds(), lane, nextBeat, JudgementName(result),
                           session.CurrentStreak(), engine.IsPlaying(h) ? "true" : "false",
                           engine.GetPositionSeconds(h), session.Clock().ElapsedSeconds());
                }
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats = DurationForLaneNote(clip, lane, nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (targetSeen && GetTickCount() - targetSeenAtTick > 6000)
        {
            printf("Stopping - target clip's section observed for 6s.\n");
            break;
        }

        if (GetTickCount() - startTick > 120000)
        {
            printf("TIMEOUT after 120s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("Final phase: %ls\n", PhaseName(session.Phase()));

    session.Stop();
    engine.Shutdown();
    return 0;
}
