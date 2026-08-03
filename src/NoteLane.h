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

// One spark from the notes-exploding burst spawned when a locked-in
// track's dots actually hand off to the next clip's preview: flies
// straight outward (with drag, so it decelerates rather than flying off
// forever) from wherever a specific note was at that instant, fading out
// over its fixed lifetime - one burst per note still on screen right
// then, not one generic burst across the lane like the confetti above.
struct ExplosionParticle
{
    double x = 0.0;
    double y = 0.0;
    double velX = 0.0;
    double velY = 0.0;
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
// tracked as spawned objects. When a track locks in, its notes keep their
// usual held/hit/miss/lane color but gain a glowing green outline, and
// keep coming (and being judged) until the next clip's own dots are due.
// A confetti burst plays across the lane the instant a track locks in;
// when the handoff to the next clip's preview actually happens, whatever
// notes are still on screen right then burst apart at their own
// positions instead (see ExplosionParticle) rather than just vanishing.
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

    // Tracks the current Learn section's GameSession::IsLockedIn() from the
    // previous frame, so the moment it flips false->true (the instant a
    // track locks in) can be caught exactly once, to trigger a confetti
    // burst right then.
    bool m_prevLockedIn = false;

    // Tracks Draw()'s locally-computed nextClipShowing (the moment a
    // locked-in track's dots actually give way to the next clip's preview)
    // from the previous frame, so that transition also gets a confetti
    // burst - without this, whatever notes are still on screen at that
    // exact instant (very likely some, since dots keep coming right up
    // until this moment) just vanish with no visual treatment at all,
    // reading as a bug ("the notes disappeared") rather than an intentional
    // handoff to the next instrument.
    bool m_prevNotesHandoff = false;
    DWORD m_confettiStartMs = 0;
    std::vector<ConfettiPiece> m_confetti;

    // The notes-exploding burst (see ExplosionParticle) fired at the
    // m_prevNotesHandoff edge instead of another confetti burst.
    DWORD m_explosionStartMs = 0;
    std::vector<ExplosionParticle> m_explosion;

    std::vector<JudgementRipple> m_ripples;
};
