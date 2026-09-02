#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic: drives GameSession headlessly through an entire chart's Learn sections
// (count-in to GamePhase::Complete), auto-pressing every note on time, and prints each section's
// clip name, each lane's anchor beat (and how far into its pattern it lands), and every judgement.
// Pass a clip name as argv[1] to focus the per-press print lines on it. Build with
// -DRHYTHM_DEBUG_JUDGEMENTS for GameSession's internal judgements too.

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

    bool heldByUs[c_LaneCount] = {};
    double releaseAtSeconds[c_LaneCount] = {};
    double lastPressedBeat[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    const wchar_t* targetClipName = L"melodica";
    static std::wstring targetClipStorage;
    if (argc > 2)
    {
        std::string arg = argv[2];
        targetClipStorage.assign(arg.begin(), arg.end());
        targetClipName = targetClipStorage.c_str();
    }

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        GamePhase phase = session.Phase();
        int sectionIndex = session.CurrentSectionIndex();
        SectionKind kind = session.CurrentSectionKind();
        double secondsPerBeat = 60.0 / session.Song().Bpm();

        bool sectionChangedThisTick = (phase != lastPhase || sectionIndex != lastSection);
        if (sectionChangedThisTick)
        {
            lastPhase = phase;
            lastSection = sectionIndex;
            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.3fs] phase=%ls section=%d kind=%d clip=(%ls)\n", session.Clock().ElapsedSeconds(),
                   PhaseName(phase), sectionIndex, static_cast<int>(kind), clip ? clip->Name().c_str() : L"-");
            if (clip && phase == GamePhase::Learning && kind == SectionKind::Learn)
            {
                // A live-clock approximation of this clip's origin - exact for a first-ever
                // appearance, a hair late for a reused clip, fine for eyeballing.
                double sectionStartBeat = session.Clock().BeatPosition();
                StemHandle stem = session.DebugStemHandle(session.Song().Sections()[sectionIndex].clipIndex);
                printf("  spanBeats=%.4f sectionStartBeat=%.4f audioPhaseSeconds=%.4f (should be ~0 for a "
                       "first-ever appearance)\n",
                       clip->SpanBeats(), sectionStartBeat, engine.GetPositionSeconds(stem));
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    if (clip->LaneNotes(lane).empty())
                    {
                        continue;
                    }
                    double anchor = session.NextExpectedBeatForLane(lane);
                    double beatsAfterSectionStart = anchor - sectionStartBeat;
                    printf("  lane %d anchor=%.4f (%.4f beats after this section began, %.1f%% of one pattern "
                           "span)\n",
                           lane, anchor, beatsAfterSectionStart, 100.0 * beatsAfterSectionStart / clip->SpanBeats());
                }
            }
        }

        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (lastSection >= 0 && session.CurrentClip() &&
                    session.CurrentClip()->Name() == targetClipName)
                {
                    printf("[t=%.3fs]   lane %d release -> %ls (streak=%d) stemPlaying=%s stemPos=%.4fs\n",
                           session.Clock().ElapsedSeconds(), lane, JudgementName(result), session.CurrentStreak(),
                           engine.IsPlaying(session.DebugStemHandle(session.Song().Sections()[lastSection].clipIndex))
                               ? "true"
                               : "false",
                           engine.GetPositionSeconds(
                               session.DebugStemHandle(session.Song().Sections()[lastSection].clipIndex)));
                }
                heldByUs[lane] = false;
            }
        }

        // Sections now always advance on a fixed schedule regardless of
        // lock-in (no more "awaiting advance" gate) - live for a Learn
        // section's whole duration.
        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn;
        if (judgingLive)
        {
            const ChartClip& clip = session.Song().Clips()[session.Song().Sections()[sectionIndex].clipIndex];
            bool isTarget = clip.Name() == targetClipName;
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                if (clip.LaneNotes(lane).empty() || heldByUs[lane])
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
                    StemHandle h = session.DebugStemHandle(sectionIndex >= 0 ? session.Song().Sections()[sectionIndex].clipIndex : -1);
                    printf("[t=%.3fs]   lane %d press beat=%.4f -> %ls (streak=%d) stemPlaying=%s stemPos=%.4fs "
                           "nowElapsed=%.4fs\n",
                           session.Clock().ElapsedSeconds(), lane, nextBeat, JudgementName(result),
                           session.CurrentStreak(), engine.IsPlaying(h) ? "true" : "false",
                           engine.GetPositionSeconds(h), session.Clock().ElapsedSeconds());
                }
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats =
                        DiagTestHelpers::DurationForLaneNote(clip, lane, session.CurrentClipOriginBeat(), nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (GetTickCount() - startTick > 500000)
        {
            printf("TIMEOUT after 500s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("Final phase: %ls\n", PhaseName(session.Phase()));

    session.Stop();
    engine.Shutdown();
    return 0;
}
