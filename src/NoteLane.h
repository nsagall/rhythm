#pragma once

#include <memory>
#include <windows.h>

#include "GameSession.h"    // JudgementResult and GameSession::HudField enums are used by value below.
#include "NoteLaneModel.h"  // NoteLaneModel m_model is a member.

class INoteLaneRenderer;

// Owns the note lane's Win32 timer/repaint plumbing and wires NoteLaneModel (game-rule logic) to
// an INoteLaneRenderer (visuals), neither of which knows the other exists. Swap m_renderer to
// change the entire visual style with zero changes to NoteLaneModel or this class.
class NoteLane
{
public:
    NoteLane();
    ~NoteLane();

    // Stores the window that owns this lane's timer and repaints.
    void Attach(HWND hwnd);

    // Sets the screen-space rect the lane is drawn and animated within.
    void SetLaneRect(RECT rect);

    // Sets the screen-space rect the hits meter panel is drawn within - separate from the lane
    // rect, since MainWindow positions it beside the playfield.
    void SetHitsMeterRect(RECT rect);

    // Starts the redraw timer.
    void StartAnimating();

    // Stops the redraw timer.
    void StopAnimating();

    // Handles a WM_TIMER tick; returns true if it belonged to this lane.
    bool OnTimer(WPARAM timerId);

    // Forwards a fresh press/release judgement to the renderer - see INoteLaneRenderer::OnJudgement.
    void ShowJudgement(JudgementResult result, int lane, bool passing, bool precise);

    // Forwards a HUD value change to the renderer - see
    // INoteLaneRenderer::OnHudValueChanged.
    void ShowHudValueChanged(GameSession::HudField field, int newValue);

    // Builds this frame's scene from session and renders it.
    void Draw(HDC hdc, const GameSession& session);

    // Toggles the renderer's clip-name debug overlay - see INoteLaneRenderer::ToggleDebugOverlay.
    void ToggleDebugOverlay();

private:
    HWND m_hwnd = nullptr;
    RECT m_laneRect{};
    RECT m_hitsMeterRect{};

    NoteLaneModel m_model;
    std::unique_ptr<INoteLaneRenderer> m_renderer;
};
