#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic: verifies "a learn clip not locked in by the time it would end repeats
// instead of advancing". Drives section 0 of a real chart, pressing nothing for the first two loop
// boundaries - confirming PendingAdvanceAtSeconds() pushes back a full loop each time while the
// section index stays put and nothing is previewed - then plays perfectly, confirming IsPassing()
// flips true mid-loop, the preview starts reflecting the real next clip, and the section then
// advances at the next loop boundary.

namespace
{


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

    bool heldByUs[c_LaneCount] = {};
    double releaseAtSeconds[c_LaneCount] = {};
    double lastPressedBeat[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    double lastSeenAdvance = -1.0;
    int extensionsObserved = 0;
    bool startPressing = false;
    bool sawPreviewBeforeLockIn = false;
    bool sawNoPreviewBeforeLockIn = false;
    bool passingObserved = false;
    double passingAtExtensions = -1;
    bool advancedObserved = false;
    int lastSectionIndex = -1;

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            if (lastSectionIndex == 0 && sectionIndex == 1)
            {
                advancedObserved = true;
                printf("[t=%.3fs] section 0 -> 1 (advanced) passing=%s\n", session.Clock().ElapsedSeconds(),
                       session.IsPassing() ? "true" : "false");
                break;
            }
            lastSectionIndex = sectionIndex;
        }

        if (sectionIndex != 0)
        {
            continue;
        }

        double advance = session.PendingAdvanceAtSeconds();
        if (lastSeenAdvance >= 0.0 && advance > lastSeenAdvance + 1e-6)
        {
            extensionsObserved++;
            printf("[t=%.3fs] section 0 REPEATED (extension #%d): advance %.3f -> %.3f, passing=%s\n",
                   session.Clock().ElapsedSeconds(), extensionsObserved, lastSeenAdvance, advance,
                   session.IsPassing() ? "true" : "false");
            if (extensionsObserved >= 2 && !startPressing)
            {
                startPressing = true;
                printf("[t=%.3fs] starting to play perfectly now\n", session.Clock().ElapsedSeconds());
            }
        }
        lastSeenAdvance = advance;

        // Track whether the preview ever shows anything before lock-in
        // (it shouldn't), and whether it shows something once locked in
        // (it should).
        bool hasPreview = session.PreviewClip() != nullptr;
        if (!session.IsPassing() && hasPreview)
        {
            sawPreviewBeforeLockIn = true;
        }
        if (!session.IsPassing() && !hasPreview)
        {
            sawNoPreviewBeforeLockIn = true;
        }
        if (session.IsPassing() && !passingObserved)
        {
            passingObserved = true;
            passingAtExtensions = extensionsObserved;
            printf("[t=%.3fs] LOCKED IN (after %d repeat(s)) streak=%d preview=%s\n", session.Clock().ElapsedSeconds(),
                   extensionsObserved, session.CurrentStreak(), hasPreview ? "non-null" : "null");
        }

        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                session.ConsumeLastJudgement();
                heldByUs[lane] = false;
            }
        }

        if (startPressing)
        {
            const ChartClip& clip = session.Song().Clips()[session.Song().Sections()[0].clipIndex];
            double secondsPerBeat = 60.0 / session.Song().Bpm();
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
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    double durationBeats =
                        DiagTestHelpers::DurationForLaneNote(clip, lane, session.CurrentClipOriginBeat(), nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (GetTickCount() - startTick > 90000)
        {
            printf("TIMEOUT after 90s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Extensions observed before playing: %d%s\n", extensionsObserved,
           extensionsObserved >= 2 ? "" : " ** MISMATCH - expected >= 2 **");
    printf("Preview stayed null before lock-in: %s%s\n", sawNoPreviewBeforeLockIn ? "true" : "false",
           sawNoPreviewBeforeLockIn ? "" : " ** MISMATCH - expected true **");
    printf("Preview ever showed something before lock-in (should never happen): %s%s\n",
           sawPreviewBeforeLockIn ? "true" : "false", sawPreviewBeforeLockIn ? " ** MISMATCH - expected false **" : "");
    printf("Locked in: %s (after %.0f repeat(s))%s\n", passingObserved ? "true" : "false", passingAtExtensions,
           passingObserved ? "" : " ** MISMATCH - expected true **");
    printf("Advanced to section 1: %s%s\n", advancedObserved ? "true" : "false",
           advancedObserved ? "" : " ** MISMATCH - expected true **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
