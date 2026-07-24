#pragma once

#include <windows.h>

#include "GameSession.h"

// Draws a lane with a fixed judge line; while a GameSession is learning or
// locking in an instrument, dots for that instrument's actual pattern
// onsets scroll in from the right and cross the line at the beat rate.
// Also renders hit/miss feedback at the line and a status readout
// (instrument name + streak). Positions are computed live from the
// session's SongClock each frame rather than tracked as spawned objects.
class NoteLane
{
public:
    // Stores the window that owns this lane's timer and repaints.
    void Attach(HWND hwnd);

    // Sets the screen-space rect the lane is drawn and animated within.
    void SetLaneRect(RECT rect);

    // Starts the redraw timer.
    void StartAnimating();

    // Stops the redraw timer.
    void StopAnimating();

    // Handles a WM_TIMER tick; returns true if it belonged to this lane.
    bool OnTimer(WPARAM timerId);

    // Flashes a brief hit/miss indicator at the judge line.
    void ShowJudgement(JudgementResult result);

    // Paints the lane, judge line, upcoming onset dots, and status text for the given session.
    void Draw(HDC hdc, const GameSession& session) const;

private:
    HWND m_hwnd = nullptr;
    RECT m_laneRect{};
    JudgementResult m_flashResult = JudgementResult::None;
    DWORD m_flashUntilMs = 0;
};
