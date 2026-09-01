#pragma once

#include <cmath>

#include "ChartClip.h"  // The inline helpers below call ChartClip methods and iterate LaneNote.

// Not part of the real build - a shared header the standalone *DiagMain.cpp files #include instead
// of each hand-copying a mirror of GameSession's private FindLaneNote.
namespace DiagTestHelpers
{

// Returns the lane note whose phase-within-span matches absoluteStartBeat's phase (relative to
// originBeat), or nullptr - mirrors GameSession::FindLaneNote.
inline const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat)
{
    double span = clip.SpanBeats();
    double phase = std::fmod(absoluteStartBeat - originBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }
    for (const LaneNote& note : clip.LaneNotes(lane))
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return &note;
        }
    }
    return nullptr;
}

// The matching note's durationBeats, or 0.0 if none matches - used to plan when to auto-release a
// held key.
inline double DurationForLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat)
{
    const LaneNote* note = FindLaneNote(clip, lane, originBeat, absoluteStartBeat);
    return note ? note->durationBeats : 0.0;
}

} // namespace DiagTestHelpers
