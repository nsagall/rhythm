#pragma once

#include <cmath>

#include "ChartClip.h"
#include "LaneConfig.h"

// Not part of the real build - a shared header the standalone *DiagMain.cpp
// files (see CLAUDE.md's "Diagnostics instead of a test suite") #include
// instead of each hand-copying their own private mirror of GameSession's
// private FindLaneNote (not directly callable from a diagnostic, since it's
// private to GameSession.cpp). Eight separate copies of this ~15-line
// helper had drifted apart in parameter order, and one of them
// (EasyModeGraceDiagMain.cpp's own DurationForLaneNote) had silently
// dropped the originBeat subtraction entirely - proof this copy-paste risk
// is real, not theoretical, even in test-only code.
namespace DiagTestHelpers
{

// Returns the lane note whose phase-within-span matches absoluteStartBeat's
// phase (measured relative to originBeat), or nullptr if none does -
// mirrors GameSession::FindLaneNote's real logic exactly.
inline const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat)
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat - originBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }
    for (const LaneNote& note : clip.laneNotes[lane])
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return &note;
        }
    }
    return nullptr;
}

// The matching note's own durationBeats, or 0.0 if none matches - what
// most diagnostics actually want, to plan when to auto-release a held key.
inline double DurationForLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat)
{
    const LaneNote* note = FindLaneNote(clip, lane, originBeat, absoluteStartBeat);
    return note ? note->durationBeats : 0.0;
}

} // namespace DiagTestHelpers
