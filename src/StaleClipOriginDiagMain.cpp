#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies the fix for
// "some clips move the first note to the very end" - reproduced via "A Real
// Good Time", whose [learn] clip=bass section is preceded by an EARLIER
// [background] clip=bass section that establishes bass's ClipInstance
// origin long before the learn section begins (with a [reset]/[break] in
// between that silences it). Before the fix, GameSession kept that stale
// origin around indefinitely (EnsureClipInstance only ever created an entry
// once, never re-anchored it), so the learn section's per-lane onsets
// picked up wherever that old, no-longer-audible beat grid said they'd be -
// which, unless the elapsed time since then happened to be an exact
// multiple of every clip's span, was NOT the pattern's own note 0. That
// note would then only appear once the section looped back around to it -
// i.e. moved to the end. The fix (GameSession::ForgetStaleClipOrigin,
// called from BeginSection before a new section (re)establishes a clip's
// origin) re-anchors instead whenever the clip isn't currently playing.
//
// This drives real gameplay up through an EARLIER, unrelated learn section
// (chords) and deliberately lets it repeat one extra loop before playing it
// - simulating an ordinary player who doesn't lock in on the very first
// pass - which shifts the total elapsed beats by an amount that (per this
// song's structure) is NOT a multiple of bass's own spanBeats. It then
// checks that every lane's first expected note in the bass learn section is
// still that lane's own pattern index 0.

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/A Real Good Time/A Real Good Time.chart";
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

    int bassClipIndex = -1;
    int chordsSectionIndex = -1;
    int targetSectionIndex = -1;
    for (size_t i = 0; i < session.Song().clips.size(); ++i)
    {
        if (session.Song().clips[i].name == L"bass")
        {
            bassClipIndex = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < session.Song().sections.size(); ++i)
    {
        const ChartSection& s = session.Song().sections[i];
        if (s.kind == SectionKind::Learn && s.clipIndex == bassClipIndex)
        {
            targetSectionIndex = static_cast<int>(i);
        }
        if (s.kind == SectionKind::Learn && s.clipIndex >= 0 && session.Song().clips[s.clipIndex].name == L"chords")
        {
            chordsSectionIndex = static_cast<int>(i);
        }
    }
    if (targetSectionIndex < 0 || chordsSectionIndex < 0)
    {
        printf("Could not find the expected [learn] clip=bass / clip=chords sections in the chart\n");
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

    bool skippedChordsFirstLoop = false;
    double chordsFirstSeenAdvance = -1.0;
    int lastSectionIndex = -2;
    bool dumped = false;
    bool allMatchedFirstNote = true;
    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete && !dumped)
    {
        session.Update();
        session.CatchUpCountIn();

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex != lastSectionIndex)
        {
            lastSectionIndex = sectionIndex;

            if (sectionIndex == targetSectionIndex)
            {
                const ChartClip& clip = session.Song().clips[bassClipIndex];
                double originBeat = session.CurrentClipOriginBeat();
                double span = clip.spanBeats;

                printf("Reached bass learn section (index %d): originBeat=%.6f spanBeats=%.6f\n", targetSectionIndex,
                       originBeat, span);

                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    if (clip.laneNotes[lane].empty())
                    {
                        continue;
                    }
                    double expected = session.NextExpectedBeatForLane(lane);
                    double phase = std::fmod(expected - originBeat, span);
                    if (phase < 0.0)
                    {
                        phase += span;
                    }
                    bool matchesFirstNote = std::abs(clip.laneNotes[lane].front().startBeat - phase) < 1e-6;
                    allMatchedFirstNote &= matchesFirstNote;
                    printf("  lane %d: first expected note phase=%.4f, pattern's own first note=%.4f%s\n", lane,
                           phase, clip.laneNotes[lane].front().startBeat,
                           matchesFirstNote ? "" : "  ** MISMATCH - first note is not this lane's pattern index 0 **");
                }
                dumped = true;
                break;
            }
        }

        if (sectionIndex < 0)
        {
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

        // Deliberately let the chords section repeat once (skip pressing
        // entirely for its first loop) before playing it - see this file's
        // own top comment for why.
        bool suppressPressing = false;
        if (sectionIndex == chordsSectionIndex && !skippedChordsFirstLoop)
        {
            double advance = session.PendingAdvanceAtSeconds();
            if (chordsFirstSeenAdvance < 0.0)
            {
                chordsFirstSeenAdvance = advance;
                suppressPressing = true;
            }
            else if (advance > chordsFirstSeenAdvance + 1e-6)
            {
                skippedChordsFirstLoop = true;
            }
            else
            {
                suppressPressing = true;
            }
        }

        if (!suppressPressing && session.Song().sections[sectionIndex].kind == SectionKind::Learn)
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
                double phase = std::fmod(nextBeat - originBeat, clip.spanBeats);
                if (phase < 0.0)
                {
                    phase += clip.spanBeats;
                }
                double durationBeats = 0.0;
                for (const LaneNote& note : clip.laneNotes[lane])
                {
                    if (std::abs(note.startBeat - phase) < 1e-6)
                    {
                        durationBeats = note.durationBeats;
                        break;
                    }
                }

                session.OnPress(lane);
                JudgementResult result = session.ConsumeLastJudgement();
                if (result == JudgementResult::None || result == JudgementResult::Hit)
                {
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;
                }
            }
        }

        if (GetTickCount() - startTick > 180000)
        {
            printf("TIMEOUT after 180s, aborting (reached section %d)\n", sectionIndex);
            break;
        }

        Sleep(2);
    }

    session.Stop();
    engine.Shutdown();

    printf("\n=== RESULTS ===\n");
    printf("Reached target section: %s\n", dumped ? "true" : "false");
    printf("Every lane's first expected note was its pattern's own note 0: %s%s\n",
           allMatchedFirstNote ? "true" : "false", allMatchedFirstNote ? "" : "  ** FAIL **");

    return (dumped && allMatchedFirstNote) ? 0 : 1;
}
