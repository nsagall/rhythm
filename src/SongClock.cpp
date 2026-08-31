#include "SongClock.h"

#include <cmath>

void SongClock::Start(double bpm)
{
    m_bpm = bpm;
    m_paused = false;
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_startTime);
}

double SongClock::ElapsedSeconds() const
{
    if (m_paused)
    {
        return m_pausedElapsedSeconds;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - m_startTime.QuadPart) / static_cast<double>(m_frequency.QuadPart);
}

double SongClock::BeatPosition() const
{
    double secondsPerBeat = 60.0 / m_bpm;
    return ElapsedSeconds() / secondsPerBeat;
}

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

void SongClock::Resync(double knownElapsedSeconds)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG offsetTicks = static_cast<LONGLONG>(knownElapsedSeconds * m_frequency.QuadPart);
    m_startTime.QuadPart = now.QuadPart - offsetTicks;
}

double SongClock::Bpm() const
{
    return m_bpm;
}

void SongClock::Pause()
{
    if (m_paused)
    {
        return;
    }
    m_pausedElapsedSeconds = ElapsedSeconds();
    m_paused = true;
}

void SongClock::Resume()
{
    if (!m_paused)
    {
        return;
    }
    m_paused = false;
    Resync(m_pausedElapsedSeconds);
}

bool SongClock::IsPaused() const
{
    return m_paused;
}
