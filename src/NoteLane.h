#pragma once

#include <vector>
#include <windows.h>

#include "GameSession.h"
#include "LaneConfig.h"

// One piece of confetti spawned when a track locks in: falls from its
// starting position under simple gravity with a bit of horizontal drift,
// fading out over its fixed lifetime. Positions/colors are generated once
// at spawn time and simulated purely from elapsed time in Draw(), same as
// every other animation here.
struct ConfettiPiece
{
    double x = 0.0;
    double y = 0.0;
    double velX = 0.0;
    double velY = 0.0;
    double spinPhase = 0.0;
    COLORREF color = 0;
};

// An expanding colored ring spawned at the instant a press/release is
// judged - starts at that lane's judge-line position and grows outward
// (at its own fixed speed - a hit ripple grows faster than a miss ripple)
// until it's fully left the visible lane. A miss ripple only ever spawns
// before its track has locked in (once locked in, a miss just becomes a
// duller green note, not a fail); a hit ripple keeps spawning even after
// lock-in, since a correct press/release is still worth celebrating during
// that extended, still-judged run.
struct JudgementRipple
{
    int lane = 0;
    DWORD startMs = 0;
    COLORREF color = 0;
    double speedPxPerSec = 0.0;
    int startAlpha = 0;      // 0-255; fades further from here as the ring grows
    double fadeRate = 1.0;   // >1 reaches zero alpha before the ring is fully grown/removed
};

// Draws kLaneCount vertical columns with a shared judge line near the
// bottom; while a GameSession is learning or locking in a clip, each lane
// independently scrolls in that lane's own MIDI-derived notes - duration
// bars with a simple glyph marking each note's start - spawning at the
// top and crossing the line at the beat rate. Also renders per-lane
// hit/miss feedback at the line and a status readout (clip name). Positions
// are computed live from the session's SongClock each frame rather than
// tracked as spawned objects. When a track locks in, its notes turn green
// (a duller green if judged a miss) instead of their usual lane color, and
// a confetti burst plays once across the lane.
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

    // Flashes a brief hit/miss indicator at the judge line for one lane's
    // column, and spawns an expanding judgement ripple there: a hit ripple
    // spawns unconditionally, a miss ripple only when the track hasn't
    // locked in yet (lockedIn == false) - a locked-in miss still gets the
    // brief flash, but no ripple, since it's already shown as a duller
    // green note, not a fail.
    void ShowJudgement(JudgementResult result, int lane, bool lockedIn);

    // Paints the columns, judge line, upcoming notes, any in-progress
    // lock-in confetti/miss-ripple animations, and status text for the
    // given session. Mutates animation/flash tracking state, so this isn't const.
    void Draw(HDC hdc, const GameSession& session);

private:
    HWND m_hwnd = nullptr;
    RECT m_laneRect{};
    JudgementResult m_flashResult[kLaneCount] = {};
    DWORD m_flashUntilMs[kLaneCount] = {};

    // Tracks the current Learn section's GameSession::IsAwaitingAdvance()
    // from the previous frame, so the moment it flips false->true (the
    // instant a track locks in) can be caught exactly once, to trigger the
    // confetti burst right then - not later, when the track's dots
    // eventually give way to the next clip's preview (nextClipShowing in
    // Draw()), which is a separate, unrelated transition.
    bool m_prevLockedIn = false;
    DWORD m_confettiStartMs = 0;
    std::vector<ConfettiPiece> m_confetti;

    std::vector<JudgementRipple> m_ripples;
};
