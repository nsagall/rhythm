#include "SongClock.h"

#include <cmath>

// Anchors the clock to now, at the given tempo.
void SongClock::Start(double bpm)
{
    m_bpm = bpm;
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_startTime);
}

// Seconds elapsed since Start() was called.
double SongClock::ElapsedSeconds() const
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - m_startTime.QuadPart) / static_cast<double>(m_frequency.QuadPart);
}

// Current position expressed in beats (quarter notes) since Start().
double SongClock::BeatPosition() const
{
    double secondsPerBeat = 60.0 / m_bpm;
    return ElapsedSeconds() / secondsPerBeat;
}

// Seconds until the next bar boundary, for quantizing when a newly locked-in instrument should start looping.
double SongClock::SecondsToNextBar(int beatsPerBar) const
{
    double beat = BeatPosition();
    double beatsIntoCurrentBar = std::fmod(beat, static_cast<double>(beatsPerBar));
    double beatsRemaining = beatsPerBar - beatsIntoCurrentBar;
    if (beatsRemaining >= beatsPerBar)
    {
        beatsRemaining = 0.0;
    }
    return beatsRemaining * (60.0 / m_bpm);
}

// Nudges the clock's anchor so ElapsedSeconds() matches a known-good audio playback position.
void SongClock::Resync(double knownElapsedSeconds)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG offsetTicks = static_cast<LONGLONG>(knownElapsedSeconds * m_frequency.QuadPart);
    m_startTime.QuadPart = now.QuadPart - offsetTicks;
}

// The tempo this clock was started with.
double SongClock::Bpm() const
{
    return m_bpm;
}
