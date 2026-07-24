#pragma once

#include <windows.h>

// A QueryPerformanceCounter-anchored clock for the current song's tempo and
// timeline. Used as the single source of truth for note judging, the
// visual lane, and lock-in scheduling, so nothing can desync by reading
// two different clocks.
class SongClock
{
public:
    // Anchors the clock to now, at the given tempo.
    void Start(double bpm);

    // Seconds elapsed since Start() was called.
    double ElapsedSeconds() const;

    // Current position expressed in beats (quarter notes) since Start().
    double BeatPosition() const;

    // Seconds until the next bar boundary, for quantizing when a newly
    // locked-in instrument should start looping.
    double SecondsToNextBar(int beatsPerBar) const;

    // Nudges the clock's anchor so ElapsedSeconds() matches a known-good
    // audio playback position, correcting drift between the CPU and audio
    // hardware clocks.
    void Resync(double knownElapsedSeconds);

    // The tempo this clock was started with.
    double Bpm() const;

private:
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_startTime{};
    double m_bpm = 120.0;
};
