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
    long long barIndex = static_cast<long long>(std::floor(localAfterBeat / span));

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

void FreshOnsetForAllLanes(double originBeat, const ChartClip& clip, double outBeats[kLaneCount])
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        const std::vector<LaneNote>& notes = clip.laneNotes[lane];
        double firstOffset = notes.empty() ? clip.spanBeats : notes.front().startBeat;
        outBeats[lane] = originBeat + firstOffset;
    }
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

} // namespace ChartTiming
