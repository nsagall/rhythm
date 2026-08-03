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

double NextOnsetAfter(double afterBeat, const ChartClip& clip, int lane)
{
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty())
    {
        return afterBeat + clip.spanBeats;
    }

    double span = clip.spanBeats;
    long long barIndex = static_cast<long long>(std::floor(afterBeat / span));

    for (const LaneNote& note : notes)
    {
        double candidate = barIndex * span + note.startBeat;
        if (candidate > afterBeat + 1e-9)
        {
            return candidate;
        }
    }
    return (barIndex + 1) * span + notes.front().startBeat;
}

double FirstReachableOnset(double afterBeat, const ChartClip& clip, int lane)
{
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty())
    {
        return NextOnsetAfter(afterBeat, clip, lane);
    }

    double span = clip.spanBeats;
    long long barIndex = (span > 0.0) ? static_cast<long long>(std::ceil((afterBeat - 1e-9) / span)) : 0;
    return barIndex * span + notes.front().startBeat;
}

void FirstReachableOnsetForAllLanes(double afterBeat, const ChartClip& clip, double outBeats[kLaneCount])
{
    double span = clip.spanBeats;

    double candidates[kLaneCount];
    long long minCycle = 0;
    long long maxCycle = 0;
    bool sawLaneWithNotes = false;

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        candidates[lane] = NextOnsetAfter(afterBeat, clip, lane);
        if (clip.laneNotes[lane].empty())
        {
            continue; // no real note on this lane to constrain the cycle check with
        }

        long long cycle = (span > 0.0) ? static_cast<long long>(std::floor(candidates[lane] / span)) : 0;
        if (!sawLaneWithNotes)
        {
            minCycle = maxCycle = cycle;
            sawLaneWithNotes = true;
        }
        else
        {
            minCycle = std::min(minCycle, cycle);
            maxCycle = std::max(maxCycle, cycle);
        }
    }

    if (sawLaneWithNotes && minCycle == maxCycle)
    {
        // Every lane's own next reachable note falls within the same
        // cycle - no lane is being asked to skip ahead of another, so
        // using them directly can't corrupt any authored relative timing.
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            outBeats[lane] = candidates[lane];
        }
        return;
    }

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        outBeats[lane] = FirstReachableOnset(afterBeat, clip, lane);
    }
}

double ComputeLoopFloorSeconds(double loopStartSeconds, double stemDuration, int loopCount)
{
    if (stemDuration <= 0.0)
    {
        return loopStartSeconds;
    }
    int minLoops = std::max(loopCount, 1);
    return std::ceil(loopStartSeconds / stemDuration) * stemDuration + (minLoops - 1) * stemDuration;
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

double ComputeLearnAdvanceSeconds(double sectionStartSeconds, double loopStartSeconds, double stemDuration,
                                   int loopCount, double tFallSeconds)
{
    if (stemDuration <= 0.0)
    {
        return sectionStartSeconds;
    }

    double naturalAdvance = std::ceil(sectionStartSeconds / stemDuration) * stemDuration;

    // loop_count sets a floor measured from when this clip's loop actually
    // started - not necessarily this section's own start, if the same clip
    // was already playing from an earlier, still-open section.
    // loop_count=1 (the default) always resolves to <= naturalAdvance,
    // since a clip can't have started playing after its own section began.
    double minimumAdvance = ComputeLoopFloorSeconds(loopStartSeconds, stemDuration, loopCount);

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

BreakAdvance ComputeBreakAdvance(double loopStartSeconds, double stemDuration, int requestedLoopCount,
                                  double tFallSeconds)
{
    BreakAdvance result;
    result.loopCount = std::max(requestedLoopCount, 1);

    // A break's loop_count is already known right now (unlike a learn
    // clip's, whose eventual stop time depends on future player input),
    // but loop_count alone isn't necessarily enough to cover tFallSeconds:
    // extend the loop count itself (not just the wait) until it is, or the
    // voice would self-stop early and leave real silence for whatever's
    // left of the wait instead of playing audio the whole time.
    while (stemDuration > 0.0 &&
           ComputeLoopFloorSeconds(loopStartSeconds, stemDuration, result.loopCount) - loopStartSeconds < tFallSeconds)
    {
        ++result.loopCount;
    }

    result.advanceSeconds = ComputeLoopFloorSeconds(loopStartSeconds, stemDuration, result.loopCount);
    return result;
}

double ComputeClipPhaseSeconds(double nowSeconds, const ChartClip& clip, double stemDuration, double bpm)
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

    double phase = std::fmod(nowSeconds, cycleDuration);
    if (phase < 0.0)
    {
        phase += cycleDuration;
    }
    return phase;
}

} // namespace ChartTiming
