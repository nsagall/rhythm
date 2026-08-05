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
    // locked-in clip should start looping.
    double SecondsToNextBar(int beatsPerBar) const;

    // Nudges the clock's anchor so ElapsedSeconds() matches a known-good
    // audio playback position, correcting drift between the CPU and audio
    // hardware clocks.
    void Resync(double knownElapsedSeconds);

    // The tempo this clock was started with.
    double Bpm() const;

    // Freezes ElapsedSeconds()/BeatPosition() at their current value until
    // Resume() - QueryPerformanceCounter itself never stops, so this is
    // purely a matter of ElapsedSeconds() returning a cached value instead
    // of a fresh read while paused. A no-op if already paused.
    void Pause();

    // Resumes from Pause(): re-anchors the clock (via Resync) so
    // ElapsedSeconds() continues from exactly the value it was frozen at,
    // as if no wall-clock time had passed during the pause. A no-op if not
    // currently paused.
    void Resume();

    bool IsPaused() const;

private:
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_startTime{};
    double m_bpm = 120.0;

    bool m_paused = false;
    // ElapsedSeconds() at the instant Pause() was called - what
    // ElapsedSeconds() itself returns while m_paused, and what Resume()
    // re-anchors the clock to.
    double m_pausedElapsedSeconds = 0.0;
};
