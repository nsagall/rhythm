#include <windows.h>

#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): broad version of
// StaleClipOriginDiagMain.cpp's check - ClipReuseAfterStopScanDiagMain.cpp
// found that EVERY real chart in Content/ has multiple clips reused by a
// [learn]/[background]/[break] section after having been silenced by an
// earlier [reset]/[break] - the exact shape that made "A Real Good Time"'s
// [learn] clip=bass present its pattern's notes out of order before
// GameSession::ForgetStaleClipOrigin. This drives a perfectly-played,
// real-time GameSession through every real chart in one run, and for every
// Learn section whose clip has been used (and since stopped) before,
// verifies every lane's very first expected note is genuinely that lane's
// own pattern note 0 - not just for bass, for every such reuse in every
// chart.

namespace
{

// Returns true (and prints per-lane detail) if every lane's first expected
// note in this freshly-begun Learn section is its own pattern's note 0.
bool CheckFirstNoteIsPatternZero(GameSession& session, const ChartClip& clip)
{
    double originBeat = session.CurrentClipOriginBeat();
    bool allOk = true;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (clip.laneNotes[lane].empty())
        {
            continue;
        }
        double expected = session.NextExpectedBeatForLane(lane);
        double phase = std::fmod(expected - originBeat, clip.spanBeats);
        if (phase < 0.0)
        {
            phase += clip.spanBeats;
        }
        bool ok = std::abs(clip.laneNotes[lane].front().startBeat - phase) < 1e-6;
        allOk &= ok;
        if (!ok)
        {
            wprintf(L"    lane %d MISMATCH: first expected phase=%.4f, pattern's own first note=%.4f\n", lane, phase,
                    clip.laneNotes[lane].front().startBeat);
        }
    }
    return allOk;
}

// Runs one chart to completion (or a generous timeout), pressing every note
// perfectly, checking CheckFirstNoteIsPatternZero on every reused clip's
// Learn section. Returns false if any mismatch was found or the chart
// couldn't be loaded.
bool RunChart(const std::wstring& chartPath)
{
    wprintf(L"=== %ls ===\n", chartPath.c_str());

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("  AudioEngine::Initialize failed\n");
        return false;
    }

    GameSession session(engine);
    std::wstring loadError;
    if (!session.LoadChart(chartPath, /*easyMode=*/false, loadError))
    {
        wprintf(L"  LoadChart failed: %ls\n", loadError.c_str());
        return false;
    }

    session.Start();

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    // Precomputed structurally (mirrors ClipReuseAfterStopScanDiagMain's own
    // walk exactly) - which Learn sections' own clip was already referenced
    // by an EARLIER section of ANY kind (background/learn/break), the
    // pattern that exposed the original bug (bass's [learn] preceded by an
    // earlier, unrelated [background] use). Deliberately NOT tracked via
    // CurrentSectionIndex() transitions at runtime - a Background section
    // never persists as "current" (GameSession::BeginSection recurses
    // straight through it), so that approach would silently miss exactly
    // this case, as an earlier version of this file did.
    std::unordered_set<int> learnSectionsWithEarlierClipUse;
    {
        std::unordered_set<const ChartClip*> everUsed;
        for (size_t i = 0; i < session.Song().sections.size(); ++i)
        {
            const ChartSection& section = session.Song().sections[i];
            if (section.clipIndex < 0)
            {
                continue;
            }
            const ChartClip* clip = &session.Song().clips[section.clipIndex];
            if (section.kind == SectionKind::Learn && everUsed.count(clip))
            {
                learnSectionsWithEarlierClipUse.insert(static_cast<int>(i));
            }
            if (section.kind == SectionKind::Learn || section.kind == SectionKind::Background ||
                section.kind == SectionKind::Break)
            {
                everUsed.insert(clip);
            }
        }
    }

    int lastSectionIndex = -2;
    bool anyMismatch = false;
    int reusedLearnChecks = 0;
    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();
        session.CatchUpCountIn();

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            lastSectionIndex = sectionIndex;
            if (sectionIndex >= 0 && session.Song().sections[sectionIndex].kind == SectionKind::Learn &&
                learnSectionsWithEarlierClipUse.count(sectionIndex))
            {
                const ChartClip* clip = session.CurrentClip();
                if (clip)
                {
                    ++reusedLearnChecks;
                    bool ok = CheckFirstNoteIsPatternZero(session, *clip);
                    wprintf(L"  [t=%.2fs] reused clip '%ls' Learn section (index %d): first-note check %ls\n",
                            session.Clock().ElapsedSeconds(), clip->name.c_str(), sectionIndex, ok ? L"OK" : L"** FAIL **");
                    anyMismatch |= !ok;
                }
            }
        }

        if (sectionIndex < 0)
        {
            if (GetTickCount() - startTick > 240000)
            {
                printf("  TIMEOUT after 240s\n");
                break;
            }
            Sleep(2);
            continue;
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

        if (session.Song().sections[sectionIndex].kind == SectionKind::Learn)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[sectionIndex].clipIndex];
            double secondsPerBeat = 60.0 / session.Song().bpm;
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (clip.laneNotes[lane].empty() || heldByUs[lane] || !session.IsLaneJudgeable(lane))
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
                double originBeat = session.CurrentClipOriginBeat();
                double durationBeats = DiagTestHelpers::DurationForLaneNote(clip, lane, originBeat, nextBeat);

                session.OnPress(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (GetTickCount() - startTick > 240000)
        {
            printf("  TIMEOUT after 240s\n");
            break;
        }

        Sleep(1);
    }

    session.Stop();
    engine.Shutdown();

    wprintf(L"  Reused-clip Learn sections checked: %d, all first-note-is-pattern-zero: %ls\n", reusedLearnChecks,
            anyMismatch ? L"false ** FAIL **" : L"true");
    wprintf(L"\n");
    return !anyMismatch;
}

} // namespace

int main()
{
    const wchar_t* charts[] = {
        L"Content/A Real Good Time/A Real Good Time.chart", L"Content/Cool Boy/Cool Boy.chart",
        L"Content/Melodius/Melodius.chart",                 L"Content/Better/better.chart",
        L"Content/Byte Blaster (AI Slop)/Byte Blaster.chart", L"Content/Forest/forest.chart",
        L"Content/Voltage Run (AI Slop)/Voltage Run.chart",
    };

    bool allOk = true;
    for (const wchar_t* chart : charts)
    {
        allOk &= RunChart(chart);
    }

    printf("=== OVERALL: %s ===\n", allOk ? "PASS" : "FAIL");
    return allOk ? 0 : 1;
}
