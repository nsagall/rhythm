#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "AudioEngine.h"
#include "ChartTiming.h"
#include "editor/BlockSchedule.h"

// Standalone diagnostic (not part of the normal build): verifies the
// editor's analytical scheduler (BlockSchedule::Build) doesn't have the same
// stale-origin bug GameSession::ForgetStaleClipOrigin fixed for live play.
//
// BlockSchedule.h's own top comment states the editor and the live game
// "can never compute different answers for the same chart" since both
// delegate to ChartTiming - but that invariant was silently broken when
// GameSession::ForgetStaleClipOrigin was added: it fixed a real bug in
// GameSession (a clip's stale, no-longer-playing origin getting reused by a
// later section, moving that section's first note to the very end - see "A
// Real Good Time"'s [learn] clip=bass, preceded by an earlier, since-
// silenced [background] clip=bass), but BlockSchedule.cpp independently
// reimplements the exact same "has this clip's origin been established"
// concept (ClipBuildState/ensureOriginEstablished, explicitly documented as
// mirroring GameSession's own ClipInstance) and was never updated to match -
// so the editor's timeline would keep showing the OLD, buggy answer even
// after the live game was fixed. Fixed by adding BlockSchedule.cpp's own
// forgetStaleOrigin, mirroring GameSession::ForgetStaleClipOrigin exactly.
//
// BlockSchedule::Build is a pure function of its inputs (no wall clock, no
// real presses - see its own doc comment), so unlike a live GameSession run
// this is exactly reproducible: it builds a real Schedule for "A Real Good
// Time" (loading real stem durations via AudioEngine, exactly like
// BlockPlayer::RebuildSchedule does) and checks that bass's [learn] Entry -
// preceded by an earlier, since-stopped [background] clip=bass entry - has
// originSeconds == sectionStartSeconds, i.e. it re-anchored fresh instead of
// carrying forward the earlier background entry's own (long stale)
// originSeconds.
//
// Also checks BlockSchedule::ComputeFirstPassSeconds directly - the
// function that replaced two independent, hand-duplicated copies of the
// same "pass 1 is shorter than a full loop" formula (Seek()'s own inline
// version, and BlockTimeline::LayoutXToSeconds's copy, which lacked
// Seek()'s implicit loopSeconds>0 guard). loopSeconds<=0 is exactly the
// case the old LayoutXToSeconds copy got wrong - it would let
// firstPassSeconds end up <= 0 and use it as a multiplier, collapsing
// every click across a degenerate block to audioStartSeconds instead of
// tracking the click - so this asserts the shared function returns exactly
// 0.0 for it instead, regardless of audioStartSeconds/originSeconds.
//
// Also checks ChartTiming::ExtendAdvanceForFallLeadTime directly against
// reference reimplementations of the two independent algorithms it
// replaced - GameSession::RegisterHit's old reactive while-loop
// (referenceOldReactive below) and BlockSchedule::Build's old closed-form
// ceil() formula (referenceOldClosedForm below) - across a range of
// inputs. Both old algorithms were confirmed mathematically equivalent by
// inspection before this refactor, so this doesn't catch a behavior
// change (there wasn't one) - it's a permanent regression guard: if the
// shared function is ever edited in a way that stops matching what either
// caller actually needs, this fails immediately instead of only surfacing
// as a timing glitch someone has to notice by ear or by eye.

namespace
{

// GameSession::RegisterHit's old algorithm, verbatim, before it was
// replaced by a call to ChartTiming::ExtendAdvanceForFallLeadTime.
double ReferenceOldReactive(double advanceSeconds, double referenceSeconds, double stemDuration, double tFallSeconds)
{
    if (stemDuration > 0.0)
    {
        while (advanceSeconds - referenceSeconds < tFallSeconds)
        {
            advanceSeconds += stemDuration;
        }
    }
    return advanceSeconds;
}

// BlockSchedule::Build's old algorithm, verbatim, before it was replaced
// by the same call.
double ReferenceOldClosedForm(double advanceSeconds, double referenceSeconds, double stemDuration,
                               double tFallSeconds)
{
    if (stemDuration > 0.0 && advanceSeconds - referenceSeconds < tFallSeconds)
    {
        double loopsNeeded = std::ceil((referenceSeconds + tFallSeconds - advanceSeconds) / stemDuration - 1e-9);
        advanceSeconds += std::max(1.0, loopsNeeded) * stemDuration;
    }
    return advanceSeconds;
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/A Real Good Time/A Real Good Time.chart";
    if (argc > 1)
    {
        std::string arg = argv[1];
        chartPath.assign(arg.begin(), arg.end());
    }

    ChartSong song;
    std::vector<std::wstring> loadErrors;
    if (!ChartFile::Load(chartPath, song, loadErrors))
    {
        wprintf(L"ChartFile::Load failed\n");
        for (const auto& e : loadErrors)
        {
            wprintf(L"  %ls\n", e.c_str());
        }
        return 1;
    }

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("AudioEngine::Initialize failed\n");
        return 1;
    }

    std::unordered_map<const ChartClip*, double> stemDurationsByClip;
    for (ChartClip& clip : song.clips)
    {
        StemHandle handle = engine.LoadStem(clip.wavFilePath);
        if (!handle.IsValid())
        {
            wprintf(L"Could not load stem for clip '%ls'\n", clip.name.c_str());
            return 1;
        }
        double duration = engine.GetStemDurationSeconds(handle);
        stemDurationsByClip[&clip] = duration;
        if (clip.hasMidi)
        {
            if (!ChartTiming::ClipFitsOneLoop(clip, duration, song.bpm))
            {
                wprintf(L"clip '%ls' doesn't fit one loop\n", clip.name.c_str());
                return 1;
            }
            ChartTiming::ExpandLaneNotesToFillClip(clip, duration, song.bpm);
        }
    }
    engine.Shutdown();

    BlockSchedule::Schedule schedule = BlockSchedule::Build(song, stemDurationsByClip);

    const ChartClip* bassClip = nullptr;
    for (const ChartClip& clip : song.clips)
    {
        if (clip.name == L"bass")
        {
            bassClip = &clip;
            break;
        }
    }
    if (!bassClip)
    {
        printf("Could not find bass clip\n");
        return 1;
    }

    // Entry only covers Learn/Break sections (Background never gets one -
    // see Entry's own doc comment, mirroring how a Background section never
    // persists as "current" in the live game either) - bass's earlier
    // [background] use only shows up as a VoiceWindow, not an Entry.
    bool foundLearnEntry = false;
    double learnSectionStartSeconds = -1.0;
    double learnOriginSeconds = -1.0;
    for (const BlockSchedule::Entry& entry : schedule.entries)
    {
        if (entry.clip == bassClip && entry.kind == SectionKind::Learn)
        {
            foundLearnEntry = true;
            learnSectionStartSeconds = entry.sectionStartSeconds;
            learnOriginSeconds = entry.originSeconds;
            break;
        }
    }
    if (!foundLearnEntry)
    {
        printf("Could not find a bass [learn] Entry in the schedule\n");
        return 1;
    }

    int bassVoiceWindowCount = 0;
    double earliestBassOriginSeconds = -1.0;
    for (const BlockSchedule::VoiceWindow& window : schedule.voices)
    {
        if (window.clip != bassClip)
        {
            continue;
        }
        ++bassVoiceWindowCount;
        printf("bass VoiceWindow #%d: startSeconds=%.6f stopSeconds=%.6f originSeconds=%.6f\n", bassVoiceWindowCount,
               window.startSeconds, window.stopSeconds, window.originSeconds);
        if (earliestBassOriginSeconds < 0.0)
        {
            earliestBassOriginSeconds = window.originSeconds;
        }
    }

    if (bassVoiceWindowCount < 2)
    {
        // This diagnostic's whole point depends on bass having an earlier
        // [background] voice window (long since closed by the time [learn]
        // opens a new one) whose stale origin there's something to forget -
        // if the chart changes such that [learn] is bass's very first use,
        // this check is vacuous, so fail loudly instead of silently passing.
        printf("Expected at least 2 bass VoiceWindows (an earlier [background] one, then [learn]'s own) - "
               "chart structure changed?\n");
        return 1;
    }

    bool reanchored = std::abs(learnSectionStartSeconds - learnOriginSeconds) < 1e-6;
    bool earlierOriginWasStale = std::abs(earliestBassOriginSeconds - learnOriginSeconds) > 1.0;

    // ComputeFirstPassSeconds(loopSeconds<=0) must always return exactly
    // 0.0, regardless of audioStartSeconds/originSeconds - this is what the
    // old, duplicated BlockTimeline::LayoutXToSeconds copy got wrong (see
    // this file's own top comment).
    bool firstPassSecondsOk = true;
    struct FirstPassCase
    {
        double audioStart;
        double origin;
        double loopSeconds;
    };
    const FirstPassCase kDegenerateCases[] = {
        {0.0, 0.0, 0.0},
        {123.456, 78.9, 0.0},
        {50.0, 50.0, -1.0},
    };
    for (const FirstPassCase& c : kDegenerateCases)
    {
        double result = BlockSchedule::ComputeFirstPassSeconds(c.audioStart, c.origin, c.loopSeconds);
        bool ok = result == 0.0;
        firstPassSecondsOk &= ok;
        printf("ComputeFirstPassSeconds(audioStart=%.3f, origin=%.3f, loopSeconds=%.3f) = %.6f%s\n", c.audioStart,
               c.origin, c.loopSeconds, result, ok ? "" : "  ** FAIL - expected exactly 0.0 **");
    }

    // ExtendAdvanceForFallLeadTime must match both of the independent
    // algorithms it replaced, across a range of inputs - no-op already
    // satisfied, exactly-one-extension, multiple-extensions, and
    // stemDuration<=0 (no-op regardless of gap).
    bool extendAdvanceOk = true;
    struct ExtendCase
    {
        double advanceSeconds;
        double referenceSeconds;
        double stemDuration;
        double tFallSeconds;
    };
    const ExtendCase kExtendCases[] = {
        {100.0, 90.0, 4.0, 5.0},   // gap already 10 >= tFallSeconds=5: no-op
        {100.0, 98.0, 4.0, 5.0},   // gap 2 < 5: needs exactly one 4s extension -> 104
        {100.0, 99.9, 1.0, 10.0},  // gap 0.1 < 10: needs several 1s extensions
        {100.0, 50.0, 0.0, 5.0},   // stemDuration<=0: no-op regardless of gap
        {100.0, 96.0, 4.0, 4.0},   // gap exactly equals tFallSeconds: no-op (not < )
    };
    for (const ExtendCase& c : kExtendCases)
    {
        double shared =
            ChartTiming::ExtendAdvanceForFallLeadTime(c.advanceSeconds, c.referenceSeconds, c.stemDuration, c.tFallSeconds);
        double oldReactive = ReferenceOldReactive(c.advanceSeconds, c.referenceSeconds, c.stemDuration, c.tFallSeconds);
        double oldClosedForm =
            ReferenceOldClosedForm(c.advanceSeconds, c.referenceSeconds, c.stemDuration, c.tFallSeconds);
        bool ok = std::abs(shared - oldReactive) < 1e-9 && std::abs(shared - oldClosedForm) < 1e-9;
        extendAdvanceOk &= ok;
        printf("ExtendAdvanceForFallLeadTime(advance=%.3f, ref=%.3f, stem=%.3f, tFall=%.3f) = %.6f "
               "(oldReactive=%.6f, oldClosedForm=%.6f)%s\n",
               c.advanceSeconds, c.referenceSeconds, c.stemDuration, c.tFallSeconds, shared, oldReactive,
               oldClosedForm, ok ? "" : "  ** FAIL - disagrees with an old algorithm **");
    }

    printf("\n=== RESULTS ===\n");
    printf("bass [learn] Entry: sectionStartSeconds=%.6f originSeconds=%.6f\n", learnSectionStartSeconds,
           learnOriginSeconds);
    printf("Earlier bass [background] VoiceWindow's own origin was genuinely different/stale: %s (%.6f vs %.6f)\n",
           earlierOriginWasStale ? "true" : "false", earliestBassOriginSeconds, learnOriginSeconds);
    printf("bass [learn] re-anchored instead of reusing that stale origin: %s%s\n", reanchored ? "true" : "false",
           reanchored ? "" : "  ** FAIL **");
    printf("ComputeFirstPassSeconds handles loopSeconds<=0 correctly on every case: %s%s\n",
           firstPassSecondsOk ? "true" : "false", firstPassSecondsOk ? "" : "  ** FAIL **");
    printf("ExtendAdvanceForFallLeadTime agrees with both old algorithms on every case: %s%s\n",
           extendAdvanceOk ? "true" : "false", extendAdvanceOk ? "" : "  ** FAIL **");

    return (reanchored && earlierOriginWasStale && firstPassSecondsOk && extendAdvanceOk) ? 0 : 1;
}
