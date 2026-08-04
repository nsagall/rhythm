#pragma once

#include <memory>
#include <windows.h>

#include "GameSession.h"
#include "LaneConfig.h"
#include "NoteLaneModel.h"
#include "NoteLaneRenderer.h"

// Owns the note lane's Win32 timer/repaint plumbing and wires
// NoteLaneModel (game-rule logic - what's happening) to an
// INoteLaneRenderer (visuals - how it looks), neither of which knows the
// other exists. Swap m_renderer for a different INoteLaneRenderer
// implementation to change the entire visual style with zero changes to
// NoteLaneModel or this class.
class NoteLane
{
public:
    NoteLane();

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

    // Forwards a fresh press/release judgement to the renderer, for
    // whatever transient visual feedback it wants to show.
    void ShowJudgement(JudgementResult result, int lane, bool lockedIn);

    // Builds this frame's scene from session and renders it.
    void Draw(HDC hdc, const GameSession& session);

private:
    HWND m_hwnd = nullptr;
    RECT m_laneRect{};

    NoteLaneModel m_model;
    std::unique_ptr<INoteLaneRenderer> m_renderer;
};
