#pragma once

#include <windows.h>

// A QueryPerformanceCounter-anchored clock for the current song's tempo and timeline. The single
// source of truth for note judging, the visual lane, and lock-in scheduling.
class SongClock
{
public:
    // Anchors the clock to now, at the given tempo.
    void Start(double bpm);

    // Seconds elapsed since Start() was called.
    double ElapsedSeconds() const;

    // Current position expressed in beats (quarter notes) since Start().
    double BeatPosition() const;

    // Seconds until the next bar boundary.
    double SecondsToNextBar(int beatsPerBar) const;

    // Nudges the clock's anchor so ElapsedSeconds() matches a known-good audio playback position.
    void Resync(double knownElapsedSeconds);

    // The tempo this clock was started with.
    double Bpm() const;

    // Freezes ElapsedSeconds()/BeatPosition() at their current value until Resume(). QPC never
    // stops; while paused ElapsedSeconds() just returns a cached value. A no-op if already paused.
    void Pause();

    // Resumes from Pause(): re-anchors the clock so ElapsedSeconds() continues from the value it
    // was frozen at, as if no time had passed. A no-op if not paused.
    void Resume();

    bool IsPaused() const;

private:
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_startTime{};
    double m_bpm = 120.0;

    bool m_paused = false;

    // ElapsedSeconds() at the instant Pause() was called.
    double m_pausedElapsedSeconds = 0.0;
};
