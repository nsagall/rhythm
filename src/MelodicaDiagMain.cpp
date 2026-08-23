#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): drives GameSession
// headlessly through an entire chart's Learn sections in order (from the
// count-in to GamePhase::Complete), auto-pressing every lane's note exactly
// on time, and prints each Learn section's clip name, each lane's anchor
// beat (and how far that lands into its own pattern - "how far into its own
// pattern is this anchor"), and every press/release judgement. Pass a
// second argv for a clip name to focus the per-press/release print lines on
// (still plays the whole song either way) - useful for isolating a specific
// clip's occurrence(s) when a chart reuses the same clip in multiple
// sections. Build with -DRHYTHM_DEBUG_JUDGEMENTS (see
// GameSession::RecordOnsetJudgement) to also see every judgement recorded
// internally, including ones from Update()'s own timeout paths that never
// go through an explicit press/release here.

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

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    const wchar_t* targetClipName = argc > 2 ? [&]() {
        static std::wstring t;
        std::string arg = argv[2];
        t.assign(arg.begin(), arg.end());
        return t.c_str();
    }() : L"melodica";

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
            lastPhase = phase;
            lastSection = sectionIndex;
            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.3fs] phase=%ls section=%d kind=%d clip=(%ls)\n", session.Clock().ElapsedSeconds(),
                   PhaseName(phase), sectionIndex, static_cast<int>(kind), clip ? clip->name.c_str() : L"-");
            if (clip && phase == GamePhase::Learning && kind == SectionKind::Learn)
            {
                // sectionStartBeat approximates this clip's own origin (see
                // GameSession::ClipVoice::originEstablished) - exact for a
                // first-ever appearance (established at this same instant),
                // and a live-clock read (not scheduledBeat-derived) so it's
                // only ever a hair later than the true origin for a
                // continuing/reused clip, which is fine for eyeballing here.
                double sectionStartBeat = session.Clock().BeatPosition();
                StemHandle stem = session.DebugStemHandle(session.Song().sections[sectionIndex].clipIndex);
                printf("  spanBeats=%.4f sectionStartBeat=%.4f audioPhaseSeconds=%.4f (should be ~0 for a "
                       "first-ever appearance)\n",
                       clip->spanBeats, sectionStartBeat, engine.GetPositionSeconds(stem));
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    if (clip->laneNotes[lane].empty())
                    {
                        continue;
                    }
                    double anchor = session.NextExpectedBeatForLane(lane);
                    double beatsAfterSectionStart = anchor - sectionStartBeat;
                    printf("  lane %d anchor=%.4f (%.4f beats after this section began, %.1f%% of one pattern "
                           "span)\n",
                           lane, anchor, beatsAfterSectionStart, 100.0 * beatsAfterSectionStart / clip->spanBeats);
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

        // Sections now always advance on a fixed schedule regardless of
        // lock-in (no more "awaiting advance" gate) - live for a Learn
        // section's whole duration.
        bool judgingLive = phase == GamePhase::Learning && kind == SectionKind::Learn;
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
