#include "ChartSong.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace
{
// Slop allowed between a MIDI pattern's declared length and its stem's measured audio length,
// since a real stem's duration essentially never lands exactly on a beat-derived value. Used by
// ClipFitsOneLoop and as ComputeLoopFloorSeconds's alignment-assert tolerance.
constexpr double c_ClipLengthToleranceSeconds = 0.1;

// True if wallClockSeconds sits within c_ClipLengthToleranceSeconds of one of the clip's own
// loop boundaries (a whole multiple of stemDuration from originSeconds).
bool IsOnLoopBoundary(double originSeconds, double wallClockSeconds, double stemDuration)
{
    double remainder = std::fmod(wallClockSeconds - originSeconds, stemDuration);
    if (remainder < 0.0)
    {
        remainder += stemDuration;
    }
    return remainder < c_ClipLengthToleranceSeconds || stemDuration - remainder < c_ClipLengthToleranceSeconds;
}

// Easy mode's density-thinning targets, in milliseconds (converted to beats via bpm - see
// EasyModeMsToBeats) so felt difficulty stays consistent across tempos. See ApplyEasyModeTransform.
constexpr double c_EasyModePerLaneMinGapMs = 260.0;     // ~2.3 hits/sec cap within one lane
constexpr double c_EasyModeGlobalMinGapMs = 170.0;      // tighter cross-lane cap, catches fast lane-alternating patterns
constexpr double c_EasyModeNoteDurationFloorMs = 130.0; // a kept note is never shorter than this

// Float-safety margin so a kept note's duration never overlaps the next kept note in its own lane.
constexpr double c_EasyModeSafetyEpsilonBeats = 1e-4;

// Below this, two kept notes (regardless of lane) are treated as simultaneous for chord-collapse purposes.
constexpr double c_EasyModeChordEpsilonBeats = 1e-6;

// One candidate note in ApplyEasyModeTransform's cross-lane thinning pass.
struct EasyModeNote
{
    double startBeat = 0.0;
    double durationBeats = 0.0;

    // Which lane this note came from.
    int lane = 0;
};

// Converts a millisecond target to beats at bpm.
//   ms  - duration in milliseconds.
//   bpm - song tempo.
// Returns the equivalent duration in beats.
double EasyModeMsToBeats(double ms, double bpm)
{
    return ms * bpm / 60000.0;
}

// Returns indices into startBeats to keep so no two consecutive kept notes - walking the circle,
// including the wrap from the last back to the first - start closer together than minGapBeats.
//   startBeats  - ascending note start beats, all in [0, spanBeats).
//   spanBeats   - the pattern's own cycle length.
//   minGapBeats - minimum allowed gap between consecutive kept notes.
// Returns kept indices, sorted ascending.
// Greedy, seeded right after the single largest circular gap - this leaves an
// already-adequately-spaced input completely untouched, with no wraparound fixup needed.
std::vector<int> CircularGreedyThinIndices(const std::vector<double>& startBeats, double spanBeats,
                                            double minGapBeats)
{
    int count = static_cast<int>(startBeats.size());
    if (count <= 1)
    {
        return count == 0 ? std::vector<int>{} : std::vector<int>{0};
    }

    std::vector<double> gaps(count);
    for (int i = 0; i < count; ++i)
    {
        double next = (i + 1 < count) ? startBeats[i + 1] : startBeats[0] + spanBeats;
        gaps[i] = next - startBeats[i];
    }
    int seed = static_cast<int>(std::max_element(gaps.begin(), gaps.end()) - gaps.begin());
    seed = (seed + 1) % count;

    std::vector<int> kept{seed};
    double lastKeptUnrolled = startBeats[seed];
    for (int step = 1; step < count; ++step)
    {
        int idx = (seed + step) % count;
        double unrolled = startBeats[idx] + spanBeats * ((seed + step) / count); // 0 or 1 loops wrapped
        if (unrolled - lastKeptUnrolled >= minGapBeats)
        {
            kept.push_back(idx);
            lastKeptUnrolled = unrolled;
        }
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}
} // namespace

double ChartClip::NextOnsetAfter(double originBeat, double afterBeat, int lane) const
{
    const std::vector<LaneNote>& notes = m_laneNotes[lane];
    if (notes.empty())
    {
        return afterBeat + m_spanBeats;
    }

    double span = m_spanBeats;
    double localAfterBeat = afterBeat - originBeat;
    // afterBeat is usually a previous call's returned value fed straight back in, so localAfterBeat
    // is meant to land exactly on a bar boundary. Roundoff can leave it a few ULPs below one, and
    // an unguarded floor() would drop it into the previous bar and return this call's own input
    // unchanged (an infinite loop). Nudging up before flooring is safe - real beat differences are
    // never close to 1e-9.
    long long barIndex = static_cast<long long>(std::floor((localAfterBeat + 1e-9) / span));

    for (const LaneNote& note : notes)
    {
        double candidate = barIndex * span + note.startBeat;
        if (candidate > localAfterBeat + 1e-9)
        {
            return originBeat + candidate;
        }
    }
    return originBeat + (barIndex + 1) * span + notes.front().startBeat;
}

double ChartClip::ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration,
                                           int loopCount)
{
    if (stemDuration <= 0.0)
    {
        return loopStartSeconds;
    }
    assert(IsOnLoopBoundary(originSeconds, loopStartSeconds, stemDuration) &&
           "ChartClip::ComputeLoopFloorSeconds: loopStartSeconds is not one of this clip's own loop boundaries - "
           "every clip must start on a beat that's a multiple of its own length");
    int minLoops = std::max(loopCount, 1);
    return loopStartSeconds + (minLoops - 1) * stemDuration;
}

void ChartClip::ExpandLaneNotesToFillClip(double stemDurationSeconds, double bpm)
{
    if (m_spanBeats <= 0.0 || stemDurationSeconds <= 0.0)
    {
        return;
    }

    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    if (clipBeats <= m_spanBeats + 1e-6)
    {
        return;
    }

    double originalSpan = m_spanBeats;
    // Measurement-slop tolerance (same as ClipFitsOneLoop), in beats - not the tighter roundoff
    // epsilons used elsewhere here.
    double toleranceBeats = c_ClipLengthToleranceSeconds / secondsPerBeat;
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        if (m_laneNotes[lane].empty())
        {
            continue;
        }

        std::vector<LaneNote> original = m_laneNotes[lane];
        std::vector<LaneNote> expanded;
        for (double repeatStart = 0.0; repeatStart < clipBeats - 1e-9; repeatStart += originalSpan)
        {
            // Only a repeat whose full span fits before the audio wraps gets tiled in; a partial
            // leftover is left silent. repeatStart only grows, so once one doesn't fit, none will.
            if (repeatStart + originalSpan > clipBeats + toleranceBeats)
            {
                break;
            }
            for (const LaneNote& note : original)
            {
                double absoluteStart = repeatStart + note.startBeat;
                if (absoluteStart + note.durationBeats <= clipBeats + 1e-9)
                {
                    expanded.push_back({absoluteStart, note.durationBeats});
                }
            }
        }
        m_laneNotes[lane] = std::move(expanded);
    }

    m_spanBeats = clipBeats;
}

void ChartClip::ApplyEasyModeTransform(double bpm)
{
    if (!m_hasMidi || m_spanBeats <= 0.0)
    {
        return;
    }

    double span = m_spanBeats;
    double perLaneMinGapBeats = EasyModeMsToBeats(c_EasyModePerLaneMinGapMs, bpm);
    double globalMinGapBeats = EasyModeMsToBeats(c_EasyModeGlobalMinGapMs, bpm);
    double durationFloorBeats = EasyModeMsToBeats(c_EasyModeNoteDurationFloorMs, bpm);

    // Stage 1: per-lane density thinning. Only which notes survive changes, never a startBeat, so
    // an already-adequately-spaced lane passes through untouched.
    std::vector<EasyModeNote> perLaneSurvivors[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        const std::vector<LaneNote>& notes = m_laneNotes[lane];
        if (notes.empty())
        {
            continue;
        }
        std::vector<double> starts;
        starts.reserve(notes.size());
        for (const LaneNote& note : notes)
        {
            starts.push_back(note.startBeat);
        }
        for (int index : CircularGreedyThinIndices(starts, span, perLaneMinGapBeats))
        {
            perLaneSurvivors[lane].push_back(EasyModeNote{notes[index].startBeat, notes[index].durationBeats, lane});
        }
    }

    // Stage 2: cross-lane thinning. Merge every lane's stage-1 survivors into one time-ordered
    // stream (ties broken by lane index, favoring the lowest lane, matching stage 3) and thin again
    // with a tighter gap. Catches patterns that rapidly alternate lanes, where each lane looks
    // sparse alone but the combined stream doesn't.
    std::vector<EasyModeNote> merged;
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        for (const EasyModeNote& note : perLaneSurvivors[lane])
        {
            merged.push_back(note);
        }
    }
    std::sort(merged.begin(), merged.end(), [](const EasyModeNote& a, const EasyModeNote& b)
              {
                  if (a.startBeat != b.startBeat)
                  {
                      return a.startBeat < b.startBeat;
                  }
                  return a.lane < b.lane;
              });

    std::vector<double> mergedStarts;
    mergedStarts.reserve(merged.size());
    for (const EasyModeNote& note : merged)
    {
        mergedStarts.push_back(note.startBeat);
    }
    std::vector<EasyModeNote> globallyThinned;
    globallyThinned.reserve(merged.size());
    for (int index : CircularGreedyThinIndices(mergedStarts, span, globalMinGapBeats))
    {
        globallyThinned.push_back(merged[index]);
    }

    // Stage 3: chord collapse - a defensive backstop independent of globalMinGapBeats tuning. Any
    // run of notes within c_EasyModeChordEpsilonBeats of the same beat survives only as its
    // lowest-indexed lane's note (lanes are pitch-ordered, so this keeps the chord's bass note).
    // globallyThinned is already sorted (startBeat asc, lane asc), so such a run is contiguous here.
    std::vector<EasyModeNote> collapsed;
    collapsed.reserve(globallyThinned.size());
    for (const EasyModeNote& note : globallyThinned)
    {
        if (!collapsed.empty() && note.startBeat - collapsed.back().startBeat < c_EasyModeChordEpsilonBeats)
        {
            continue; // same near-simultaneous run as the previous (lower-lane) survivor
        }
        collapsed.push_back(note);
    }

    // Stage 4: duration. Each surviving note keeps its authored length, clamped to a floor (never
    // imperceptibly short) and a ceiling of "don't run into this lane's next surviving note" -
    // computed circularly (the last note is bounded by the first, one span later). perLaneMinGapMs
    // always exceeds the floor, so the std::max below is just insurance against retuning those.
    std::vector<LaneNote> perLaneFinal[c_LaneCount];
    for (const EasyModeNote& note : collapsed)
    {
        perLaneFinal[note.lane].push_back(LaneNote{note.startBeat, note.durationBeats});
    }
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        std::vector<LaneNote>& notes = perLaneFinal[lane];
        for (size_t i = 0; i < notes.size(); ++i)
        {
            double nextStart = (i + 1 < notes.size()) ? notes[i + 1].startBeat : notes[0].startBeat + span;
            double ceiling = nextStart - notes[i].startBeat - c_EasyModeSafetyEpsilonBeats;
            notes[i].durationBeats =
                std::clamp(notes[i].durationBeats, durationFloorBeats, std::max(durationFloorBeats, ceiling));
        }
        m_laneNotes[lane] = std::move(notes);
    }
}

double ChartClip::ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double stemDuration,
                                              int loopCount, double tFallSeconds)
{
    if (stemDuration <= 0.0)
    {
        return sectionStartSeconds;
    }

    // A Learn section always restarts its clip fresh (ValidateArrangementAlignment enforces this),
    // so the loop start is sectionStartSeconds itself.
    double advanceSeconds = ComputeLoopFloorSeconds(originSeconds, sectionStartSeconds, stemDuration, loopCount);

    // Guarantee at least tFallSeconds between this section starting and the next, extended by whole
    // loops rather than a pause so the audio is never cut mid-loop.
    while (advanceSeconds - sectionStartSeconds < tFallSeconds)
    {
        advanceSeconds += stemDuration;
    }
    return advanceSeconds;
}

bool ChartClip::ClipFitsOneLoop(double stemDurationSeconds, double bpm) const
{
    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    double toleranceBeats = c_ClipLengthToleranceSeconds / secondsPerBeat;
    return clipBeats >= m_spanBeats - toleranceBeats;
}

ChartClip::BreakAdvance ChartClip::ComputeBreakAdvance(double originSeconds, double loopStartSeconds,
                                                        double stemDuration, int requestedLoopCount,
                                                        double tFallSeconds)
{
    BreakAdvance result;
    result.loopCount = std::max(requestedLoopCount, 1);

    // loop_count alone may not cover tFallSeconds. Extend the loop count itself (not just the wait)
    // until it does, or the voice self-stops early and leaves silence for the rest of the wait.
    while (stemDuration > 0.0 && ComputeLoopFloorSeconds(originSeconds, loopStartSeconds, stemDuration,
                                                           result.loopCount) - loopStartSeconds < tFallSeconds)
    {
        ++result.loopCount;
    }

    result.advanceSeconds = ComputeLoopFloorSeconds(originSeconds, loopStartSeconds, stemDuration, result.loopCount);
    return result;
}

double ChartClip::ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                                double tFallSeconds)
{
    if (stemDuration <= 0.0)
    {
        return advanceSeconds;
    }
    while (advanceSeconds - referenceSeconds < tFallSeconds)
    {
        advanceSeconds += stemDuration;
    }
    return advanceSeconds;
}

double ChartClip::ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, double stemDuration,
                                           double bpm) const
{
    double cycleDuration = stemDuration;
    if (m_hasMidi && m_spanBeats > 0.0 && bpm > 0.0)
    {
        cycleDuration = m_spanBeats * (60.0 / bpm);
    }
    if (cycleDuration <= 0.0)
    {
        return 0.0;
    }

    double phase = std::fmod(nowSeconds - originSeconds, cycleDuration);
    if (phase < 0.0)
    {
        phase += cycleDuration;
    }
    return phase;
}

std::vector<double> ChartClip::OnsetsInRange(double originBeat, double afterBeatExclusive, double uptoBeatInclusive,
                                              const std::vector<LaneNote>& notes, double spanBeats)
{
    std::vector<double> result;
    if (notes.empty() || spanBeats <= 0.0 || uptoBeatInclusive <= afterBeatExclusive)
    {
        return result;
    }

    // Same epsilon-nudged bar-index convention as NextOnsetAfter.
    double localAfter = afterBeatExclusive - originBeat;
    double localUpto = uptoBeatInclusive - originBeat;
    long long firstBar = static_cast<long long>(std::floor((localAfter + 1e-9) / spanBeats));
    long long lastBar = static_cast<long long>(std::floor((localUpto + 1e-9) / spanBeats));

    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (const LaneNote& note : notes)
        {
            double candidate = bar * spanBeats + note.startBeat;
            if (candidate > localAfter + 1e-9 && candidate <= localUpto + 1e-9)
            {
                result.push_back(originBeat + candidate);
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

namespace
{

// The arrangement position ValidateArrangementAlignment tracks while walking a chart's sections in
// order. "exact" means a precise beat offset is known; the ambiguous fallback only knows a divisor.
struct ArrangementWalkState
{
    bool open = false;
    bool exact = true;
    long long exactOffsetBeats = 0;
    long long ambiguousDivisorBeats = 0;
    std::unordered_map<const ChartClip*, bool> clipOpen;
};

} // namespace

bool ChartClip::ValidateArrangementAlignment(const ChartSong& song,
                                              const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                              std::vector<std::wstring>& outErrors)
{
    outErrors.clear();
    if (song.Bpm() <= 0.0 || song.BeatsPerBar() <= 0)
    {
        return true; // nothing sensible to check - ChartSong::Load already rejects this
    }
    double secondsPerBeat = song.SecondsPerBeat();
    double tFallSeconds = c_NoteFallBeats * secondsPerBeat;
    // Audio-measurement slop, needed only for drivingSpanBeats below (derived from real stem
    // duration); authoredSpanBeats is already an exact multiple of beatsPerBar.
    double toleranceBeats = c_ClipLengthToleranceSeconds / secondsPerBeat;

    // Every judged clip's two lengths, each confirmed a whole number of bars up front so every
    // later check is exact integer beats. A clip whose stem isn't longer than its authored pattern
    // has the same value in both maps.
    std::unordered_map<const ChartClip*, long long> authoredSpanBeats;
    std::unordered_map<const ChartClip*, long long> drivingSpanBeats;
    for (const ChartClip& clip : song.Clips())
    {
        auto it = clipInfo.find(&clip);
        if (!clip.HasMidi() || it == clipInfo.end())
        {
            continue; // no pattern of its own to align - see the Break case below
        }
        double authored = it->second.authoredSpanBeats;
        long long authoredBars = static_cast<long long>(std::llround(authored / song.BeatsPerBar()));
        if (authoredBars < 1 || std::abs(authoredBars * song.BeatsPerBar() - authored) > 1e-6)
        {
            outErrors.push_back(L"clip '" + clip.Name() + L"': its length (" + std::to_wstring(authored) +
                                 L" beats) is not a whole number of " + std::to_wstring(song.BeatsPerBar()) +
                                 L"-beat bars");
            continue;
        }
        authoredSpanBeats[&clip] = authoredBars * song.BeatsPerBar();

        // Mirrors ExpandLaneNotesToFillClip's no-widening-needed check, so this agrees with the
        // real loading path.
        double stemBeats = it->second.stemDurationSeconds / secondsPerBeat;
        double driving = (stemBeats <= authored + 1e-6) ? authored : stemBeats;
        long long drivingBars = static_cast<long long>(std::llround(driving / song.BeatsPerBar()));
        if (drivingBars < 1 || std::abs(drivingBars * song.BeatsPerBar() - driving) > toleranceBeats)
        {
            outErrors.push_back(L"clip '" + clip.Name() + L"': its real audio length (" + std::to_wstring(driving) +
                                 L" beats) is not a whole number of " + std::to_wstring(song.BeatsPerBar()) +
                                 L"-beat bars");
            continue;
        }
        drivingSpanBeats[&clip] = drivingBars * song.BeatsPerBar();
    }
    if (!outErrors.empty())
    {
        return false;
    }

    ArrangementWalkState state;

    // Validates a clip joining now against its AUTHORED length (no-op if it's already open, or has
    // no pattern of its own).
    auto join = [&](const ChartClip* clip, size_t sectionOrdinal)
    {
        if (state.clipOpen[clip] || !authoredSpanBeats.count(clip))
        {
            state.clipOpen[clip] = true;
            return;
        }
        long long span = authoredSpanBeats.at(clip);
        if (!state.open)
        {
            state.open = true;
            state.exact = true;
            state.exactOffsetBeats = 0;
        }
        else if (state.exact ? (state.exactOffsetBeats % span != 0) : (state.ambiguousDivisorBeats % span != 0))
        {
            outErrors.push_back(L"section #" + std::to_wstring(sectionOrdinal + 1) + L": clip '" + clip->Name() +
                                 L"' doesn't start on one of its own bar boundaries within the arrangement it's "
                                 L"joining - every clip that plays concurrently with others must be the same "
                                 L"length or a whole multiple of theirs, and (right after a Learn section "
                                 L"specifically, since its real repeat count depends on the player) never longer "
                                 L"than the section it's joining after");
        }
        state.clipOpen[clip] = true;
    };

    const ChartClip* queuedBackground = nullptr;

    for (size_t i = 0; i < song.Sections().size(); ++i)
    {
        const ChartSection& section = song.Sections()[i];
        const ChartClip* clip =
            section.clipIndex >= 0 ? &song.Clips()[static_cast<size_t>(section.clipIndex)] : nullptr;

        if (queuedBackground != nullptr)
        {
            join(queuedBackground, i);
            queuedBackground = nullptr;
        }

        switch (section.kind)
        {
            case SectionKind::Background:
                queuedBackground = clip;
                break;

            case SectionKind::Reset:
                state = ArrangementWalkState{};
                break;

            case SectionKind::Break:
            {
                bool wasOpen = state.clipOpen[clip];
                for (auto& entry : state.clipOpen)
                {
                    if (entry.first != clip)
                    {
                        entry.second = false;
                    }
                }
                if (!wasOpen)
                {
                    state.open = false; // StopAllExcept a clip that wasn't playing silences everything
                }
                join(clip, i);

                if (!clip->HasMidi())
                {
                    // No pattern to keep a bar grid against.
                    state.open = false;
                    break;
                }
                if (state.exact)
                {
                    // Real advance, computed via the shared function rather than reimplemented, so
                    // it can't drift from runtime.
                    double localLoopStart = static_cast<double>(state.exactOffsetBeats) * secondsPerBeat;
                    BreakAdvance advance = ComputeBreakAdvance(0.0, localLoopStart, clipInfo.at(clip).stemDurationSeconds,
                                                                 section.loopCount, tFallSeconds);
                    state.exactOffsetBeats = std::llround(advance.advanceSeconds / secondsPerBeat);
                }
                else
                {
                    // The DRIVING length - what the break's advance actually steps by.
                    state.ambiguousDivisorBeats = drivingSpanBeats.at(clip);
                }
                break;
            }

            case SectionKind::Learn:
                // A Learn section always restarts its clip fresh; it never joins one still open
                // from an earlier section. Enforcing that here lets ComputeLearnAdvanceSeconds
                // assume its loop start is exactly sectionStartSeconds.
                if (state.clipOpen[clip])
                {
                    outErrors.push_back(L"section #" + std::to_wstring(i + 1) + L": clip '" + clip->Name() +
                                         L"' is already playing from an earlier, still-open section - a [learn] "
                                         L"section always restarts its own clip fresh, so reusing one that's "
                                         L"already open would silently keep playing from the old instant instead; "
                                         L"insert a [break] or [reset] before this section");
                }
                join(clip, i);
                // From here until a Reset the real advance is unbounded (it depends on how many
                // loops the player needs to reach hits_required), so nothing after this can claim
                // an exact position - only a divisor of this clip's DRIVING length.
                state.exact = false;
                state.ambiguousDivisorBeats = drivingSpanBeats.at(clip);
                break;
        }
    }

    return outErrors.empty();
}

void ClipInstance::SetContext(const ChartClip& clip, double originSeconds, double stemDurationSeconds)
{
    m_clip = &clip;
    m_originSeconds = originSeconds;
    m_stemDurationSeconds = stemDurationSeconds;
}

void ClipInstance::MarkStarted(double loopStartSeconds)
{
    m_isPlaying = true;
    m_loopStartSeconds = loopStartSeconds;
}

void ClipInstance::MarkStopped()
{
    m_isPlaying = false;
}

double ClipInstance::OriginBeat(double bpm) const
{
    return m_originSeconds / (60.0 / bpm);
}

double ClipInstance::NextOnsetAfter(double bpm, double afterBeat, int lane) const
{
    return m_clip->NextOnsetAfter(OriginBeat(bpm), afterBeat, lane);
}

double ClipInstance::ComputeClipPhaseSeconds(double nowSeconds, double bpm) const
{
    return m_clip->ComputeClipPhaseSeconds(m_originSeconds, nowSeconds, m_stemDurationSeconds, bpm);
}

double ClipInstance::ComputeLearnAdvanceSeconds(double sectionStartSeconds, int loopCount, double tFallSeconds) const
{
    return ChartClip::ComputeLearnAdvanceSeconds(m_originSeconds, sectionStartSeconds, m_stemDurationSeconds,
                                                  loopCount, tFallSeconds);
}

ChartClip::BreakAdvance ClipInstance::ComputeBreakAdvance(double loopStartSeconds, int requestedLoopCount,
                                                           double tFallSeconds) const
{
    return ChartClip::ComputeBreakAdvance(m_originSeconds, loopStartSeconds, m_stemDurationSeconds,
                                           requestedLoopCount, tFallSeconds);
}

double ClipInstance::ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds,
                                                   double tFallSeconds) const
{
    return ChartClip::ExtendAdvanceForFallLeadTime(advanceSeconds, referenceSeconds, m_stemDurationSeconds,
                                                    tFallSeconds);
}

std::vector<double> ClipInstance::OnsetsInRange(double bpm, double afterBeatExclusive, double uptoBeatInclusive,
                                                 int lane) const
{
    return ChartClip::OnsetsInRange(OriginBeat(bpm), afterBeatExclusive, uptoBeatInclusive,
                                     m_clip->LaneNotes(lane), m_clip->SpanBeats());
}
