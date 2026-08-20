#include "ChartTiming.h"

#include <algorithm>
#include <cmath>

namespace ChartTiming
{

namespace
{
// How much a MIDI pattern's declared length is allowed to exceed its
// stem's measured audio length before ClipFitsOneLoop rejects it as not
// fitting in one loop. A real stem's duration (sample count / sample rate)
// will essentially never land exactly on a beat-derived value, so this has
// to be generous enough to absorb ordinary export/rounding slop rather
// than rejecting legitimately-fitting content over a couple of
// milliseconds.
constexpr double kClipLengthToleranceSeconds = 0.1;
} // namespace

double NextOnsetAfter(double originBeat, double afterBeat, const ChartClip& clip, int lane)
{
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty())
    {
        return afterBeat + clip.spanBeats;
    }

    double span = clip.spanBeats;
    double localAfterBeat = afterBeat - originBeat;
    // The +1e-9 here (not just on the "candidate > localAfterBeat" check
    // below) matters: afterBeat is usually itself a previous call's own
    // returned value (originBeat + someBarIndex * span + note.startBeat),
    // fed straight back in as the next call's afterBeat - so
    // localAfterBeat is meant to land exactly on a bar boundary here more
    // often than not, not just close to one. Ordinary floating-point
    // roundoff in that addition/subtraction round trip can leave it a few
    // ULPs *below* the boundary instead of exactly on it - and an
    // unguarded floor() rounds that down into the *previous* bar, handing
    // back the exact same beat this call was given in the first place
    // (confirmed real repro: NextOnsetAfter looping forever on one beat,
    // every call returning its own input unchanged, once local drift
    // pushed a boundary-exact value a hair under). Nudging up first before
    // flooring is safe - real beat differences are never anywhere close to
    // 1e-9 - and keeps this in the same bar the unguarded "candidate >
    // localAfterBeat + 1e-9" comparison below already treats it as being in.
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

double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration, int loopCount)
{
    if (stemDuration <= 0.0)
    {
        return loopStartSeconds;
    }
    int minLoops = std::max(loopCount, 1);
    double localLoopStart = loopStartSeconds - originSeconds;
    return originSeconds + std::ceil(localLoopStart / stemDuration) * stemDuration + (minLoops - 1) * stemDuration;
}

void ExpandLaneNotesToFillClip(ChartClip& clip, double stemDurationSeconds, double bpm)
{
    if (clip.spanBeats <= 0.0 || stemDurationSeconds <= 0.0)
    {
        return;
    }

    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    if (clipBeats <= clip.spanBeats + 1e-6)
    {
        return;
    }

    double originalSpan = clip.spanBeats;
    // Same tolerance ClipFitsOneLoop uses, in beats - a real stem's measured
    // duration essentially never lands exactly on a beat-derived value, so
    // "does this whole repeat fit" needs the same measurement-slop
    // allowance, not the tighter floating-point-safety epsilons used
    // elsewhere below (which are about roundoff, not measurement noise).
    double toleranceBeats = kClipLengthToleranceSeconds / secondsPerBeat;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (clip.laneNotes[lane].empty())
        {
            continue;
        }

        std::vector<LaneNote> original = clip.laneNotes[lane];
        std::vector<LaneNote> expanded;
        for (double repeatStart = 0.0; repeatStart < clipBeats - 1e-9; repeatStart += originalSpan)
        {
            // Only a repeat whose own full span entirely fits before the
            // audio wraps gets tiled in - see this function's own header
            // comment for why a partial leftover is left silent instead.
            // repeatStart only ever grows from here, so once one repeat
            // doesn't fit, no later one will either.
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
        clip.laneNotes[lane] = std::move(expanded);
    }

    clip.spanBeats = clipBeats;
}

double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double loopStartSeconds,
                                   double stemDuration, int loopCount, double tFallSeconds)
{
    if (stemDuration <= 0.0)
    {
        return sectionStartSeconds;
    }

    double localSectionStart = sectionStartSeconds - originSeconds;
    double naturalAdvance = originSeconds + std::ceil(localSectionStart / stemDuration) * stemDuration;

    // loop_count sets a floor measured from when this clip's loop actually
    // started - not necessarily this section's own start, if the same clip
    // was already playing from an earlier, still-open section.
    // loop_count=1 (the default) always resolves to <= naturalAdvance,
    // since a clip can't have started playing after its own section began.
    double minimumAdvance = ComputeLoopFloorSeconds(originSeconds, loopStartSeconds, stemDuration, loopCount);

    double advanceSeconds = std::max(naturalAdvance, minimumAdvance);

    // Guarantee at least a full tFallSeconds of real time between the
    // section starting and the next one actually starting, extended by
    // whole extra loops rather than an arbitrary pause, so the current
    // clip's audio never gets cut mid-loop.
    while (advanceSeconds - sectionStartSeconds < tFallSeconds)
    {
        advanceSeconds += stemDuration;
    }
    return advanceSeconds;
}

bool ClipFitsOneLoop(const ChartClip& clip, double stemDurationSeconds, double bpm)
{
    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    double toleranceBeats = kClipLengthToleranceSeconds / secondsPerBeat;
    return clipBeats >= clip.spanBeats - toleranceBeats;
}

BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                  int requestedLoopCount, double tFallSeconds)
{
    BreakAdvance result;
    result.loopCount = std::max(requestedLoopCount, 1);

    // A break's loop_count is already known right now (unlike a learn
    // clip's, whose eventual stop time depends on future player input),
    // but loop_count alone isn't necessarily enough to cover tFallSeconds:
    // extend the loop count itself (not just the wait) until it is, or the
    // voice would self-stop early and leave real silence for whatever's
    // left of the wait instead of playing audio the whole time.
    while (stemDuration > 0.0 && ComputeLoopFloorSeconds(originSeconds, loopStartSeconds, stemDuration,
                                                           result.loopCount) - loopStartSeconds < tFallSeconds)
    {
        ++result.loopCount;
    }

    result.advanceSeconds = ComputeLoopFloorSeconds(originSeconds, loopStartSeconds, stemDuration, result.loopCount);
    return result;
}

double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
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

double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, const ChartClip& clip, double stemDuration,
                                double bpm)
{
    double cycleDuration = stemDuration;
    if (clip.hasMidi && clip.spanBeats > 0.0 && bpm > 0.0)
    {
        cycleDuration = clip.spanBeats * (60.0 / bpm);
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

std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive, double uptoBeatInclusive,
                                   const std::vector<LaneNote>& notes, double spanBeats)
{
    std::vector<double> result;
    if (notes.empty() || spanBeats <= 0.0 || uptoBeatInclusive <= afterBeatExclusive)
    {
        return result;
    }

    // Same epsilon-nudged bar-index convention as NextOnsetAfter - see its
    // own comment for why an unguarded floor() is unsafe here.
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

// Tracks the arrangement position ValidateArrangementAlignment needs while
// walking a chart's sections in order - see the header's own comment for
// what "exact" vs. the ambiguous fallback mean and why.
struct ArrangementWalkState
{
    bool open = false;
    bool exact = true;
    long long exactOffsetBeats = 0;
    long long ambiguousDivisorBeats = 0;
    std::unordered_map<const ChartClip*, bool> clipOpen;
};

} // namespace

bool ValidateArrangementAlignment(const ChartSong& song,
                                   const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                   std::vector<std::wstring>& outErrors)
{
    outErrors.clear();
    if (song.bpm <= 0.0 || song.beatsPerBar <= 0)
    {
        return true; // nothing sensible to check - ChartFile::Load already rejects this
    }
    double secondsPerBeat = 60.0 / song.bpm;
    double tFallSeconds = kNoteFallBeats * secondsPerBeat;
    // Same slop ExpandLaneNotesToFillClip/ClipFitsOneLoop already tolerate
    // between a clip's declared beat length and its real, audio-measured one
    // - needed only for drivingSpanBeats below (derived from real stem
    // duration), never for authoredSpanBeats (ChartFile::Load's
    // AlignToBarBoundary output, already an exact whole multiple of
    // beatsPerBar with no audio-measurement slop of its own).
    double toleranceBeats = kClipLengthToleranceSeconds / secondsPerBeat;

    // Every judged clip's two lengths, each confirmed a whole number of bars
    // up front so every later check is exact integer beats, never float bar
    // division - see ClipAlignmentInfo's own comment for why a clip needs
    // both, and which one governs which role below. A clip whose stem isn't
    // longer than its authored pattern (no ExpandLaneNotesToFillClip
    // widening would happen) has an identical value in both maps.
    std::unordered_map<const ChartClip*, long long> authoredSpanBeats;
    std::unordered_map<const ChartClip*, long long> drivingSpanBeats;
    for (const ChartClip& clip : song.clips)
    {
        auto it = clipInfo.find(&clip);
        if (!clip.hasMidi || it == clipInfo.end())
        {
            continue; // no pattern of its own to align - see the Break case below
        }
        double authored = it->second.authoredSpanBeats;
        long long authoredBars = static_cast<long long>(std::llround(authored / song.beatsPerBar));
        if (authoredBars < 1 || std::abs(authoredBars * song.beatsPerBar - authored) > 1e-6)
        {
            outErrors.push_back(L"clip '" + clip.name + L"': its length (" + std::to_wstring(authored) +
                                 L" beats) is not a whole number of " + std::to_wstring(song.beatsPerBar) +
                                 L"-beat bars");
            continue;
        }
        authoredSpanBeats[&clip] = authoredBars * song.beatsPerBar;

        // Mirrors ExpandLaneNotesToFillClip's own no-widening-needed check
        // exactly, so this always agrees with whatever the real loading
        // path actually does with this clip.
        double stemBeats = it->second.stemDurationSeconds / secondsPerBeat;
        double driving = (stemBeats <= authored + 1e-6) ? authored : stemBeats;
        long long drivingBars = static_cast<long long>(std::llround(driving / song.beatsPerBar));
        if (drivingBars < 1 || std::abs(drivingBars * song.beatsPerBar - driving) > toleranceBeats)
        {
            outErrors.push_back(L"clip '" + clip.name + L"': its real audio length (" + std::to_wstring(driving) +
                                 L" beats) is not a whole number of " + std::to_wstring(song.beatsPerBar) +
                                 L"-beat bars");
            continue;
        }
        drivingSpanBeats[&clip] = drivingBars * song.beatsPerBar;
    }
    if (!outErrors.empty())
    {
        return false;
    }

    ArrangementWalkState state;

    // Validates clip joining right now (a no-op if it's already open, i.e.
    // continuing an unbroken groove - already validated when it first
    // joined) against its own AUTHORED length (see ClipAlignmentInfo's own
    // comment for why the joining role uses this one, not drivingSpanBeats).
    // Every clip without a pattern of its own (authoredSpanBeats has no
    // entry) trivially passes - nothing to misalign.
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
            outErrors.push_back(L"section #" + std::to_wstring(sectionOrdinal + 1) + L": clip '" + clip->name +
                                 L"' doesn't start on one of its own bar boundaries within the arrangement it's "
                                 L"joining - every clip that plays concurrently with others must be the same "
                                 L"length or a whole multiple of theirs, and (right after a Learn section "
                                 L"specifically, since its real repeat count depends on the player) never longer "
                                 L"than the section it's joining after");
        }
        state.clipOpen[clip] = true;
    };

    const ChartClip* queuedBackground = nullptr;

    for (size_t i = 0; i < song.sections.size(); ++i)
    {
        const ChartSection& section = song.sections[i];
        const ChartClip* clip = section.clipIndex >= 0 ? &song.clips[static_cast<size_t>(section.clipIndex)] : nullptr;

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

                if (!clip->hasMidi)
                {
                    // No pattern to keep a bar grid against - nothing after
                    // this can assume an exact position relative to it.
                    state.open = false;
                    break;
                }
                if (state.exact)
                {
                    // Real advance, exactly as the live game/editor compute
                    // it - not reimplemented here, so this can never drift
                    // from what actually happens at runtime.
                    double localLoopStart = static_cast<double>(state.exactOffsetBeats) * secondsPerBeat;
                    BreakAdvance advance = ComputeBreakAdvance(0.0, localLoopStart, clipInfo.at(clip).stemDurationSeconds,
                                                                 section.loopCount, tFallSeconds);
                    state.exactOffsetBeats = std::llround(advance.advanceSeconds / secondsPerBeat);
                }
                else
                {
                    // The DRIVING length, not the authored one - this is
                    // what the break's own advance actually steps by (see
                    // ClipAlignmentInfo's own comment).
                    state.ambiguousDivisorBeats = drivingSpanBeats.at(clip);
                }
                break;
            }

            case SectionKind::Learn:
                join(clip, i);
                // From here until a Reset, the real advance depends on
                // however many extra loops this player needs to reach
                // hits_required - unbounded, so nothing after this can claim
                // an exact position again, only a divisor of this clip's own
                // DRIVING length (see join's own comment, and
                // ClipAlignmentInfo's, for why this is deliberately not the
                // same length join() itself just checked this clip against).
                state.exact = false;
                state.ambiguousDivisorBeats = drivingSpanBeats.at(clip);
                break;
        }
    }

    return outErrors.empty();
}

} // namespace ChartTiming
