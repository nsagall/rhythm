#pragma once

#include <windows.h>

#include <vector>

// Draws a lane with a fixed vertical line; while playing, dots spawn at the
// right edge and travel left at a constant speed timed so each one crosses
// the line exactly at the given BPM.
class BeatScroller
{
public:
    // Stores the window that owns this scroller's timers and repaints.
    void Attach(HWND hwnd);

    // Sets the screen-space rect the lane is drawn and animated within.
    void SetLaneRect(RECT rect);

    // Begins spawning and animating dots at the given beats-per-minute rate.
    void Start(int bpm);

    // Stops animating and clears all dots.
    void Stop();

    // Handles a WM_TIMER tick; returns true if it belonged to this scroller.
    bool OnTimer(WPARAM timerId);

    // Paints the lane, its line, and all currently visible dots.
    void Draw(HDC hdc) const;

private:
    // Records a new dot starting its journey from the right edge now.
    void SpawnDot();

    // Returns the x-coordinate of the vertical line within the lane.
    int GetLineX() const;

    HWND m_hwnd = nullptr;
    RECT m_laneRect{};
    std::vector<DWORD> m_dotSpawnTimes;
    double m_speedPxPerMs = 0.0;
    DWORD m_dotLifetimeMs = 0;
    bool m_isPlaying = false;
};
