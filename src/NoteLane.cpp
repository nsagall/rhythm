#include "NoteLane.h"

#include "NoteLaneGdiRenderer.h"

namespace
{
constexpr UINT_PTR kFrameTimerId = 400;
constexpr UINT kFrameIntervalMs = 16;
} // namespace

// Defaults to the GDI renderer - the only concrete INoteLaneRenderer this
// codebase has today. Only this constructor needs to know that; everything
// else in this class talks to m_renderer purely through the interface.
NoteLane::NoteLane() : m_renderer(std::make_unique<NoteLaneGdiRenderer>())
{
}

void NoteLane::Attach(HWND hwnd)
{
    m_hwnd = hwnd;
}

void NoteLane::SetLaneRect(RECT rect)
{
    m_laneRect = rect;
}

void NoteLane::SetHitsMeterRect(RECT rect)
{
    m_hitsMeterRect = rect;
}

void NoteLane::StartAnimating()
{
    SetTimer(m_hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);
}

void NoteLane::StopAnimating()
{
    KillTimer(m_hwnd, kFrameTimerId);
}

bool NoteLane::OnTimer(WPARAM timerId)
{
    if (timerId != kFrameTimerId)
    {
        return false;
    }
    // The hits meter panel sits outside m_laneRect (see MainWindow::Layout),
    // so both rects need to stay invalidated every animating frame, not
    // just the lane itself - otherwise its fill/burst would only actually
    // reach the screen whenever something else happened to invalidate the
    // whole client area.
    RECT dirtyRect{};
    UnionRect(&dirtyRect, &m_laneRect, &m_hitsMeterRect);
    InvalidateRect(m_hwnd, &dirtyRect, FALSE);
    return true;
}

void NoteLane::ShowJudgement(JudgementResult result, int lane, bool passing, bool precise)
{
    m_renderer->OnJudgement(result, lane, passing, precise);
}

void NoteLane::ShowScoreBanked(int amount)
{
    m_renderer->OnScoreBanked(amount);
}

void NoteLane::ShowScoreLost(int amount)
{
    m_renderer->OnScoreLost(amount);
}

void NoteLane::Draw(HDC hdc, const GameSession& session)
{
    NoteLaneScene scene = m_model.BuildScene(session);
    m_renderer->Draw(hdc, m_laneRect, m_hitsMeterRect, scene);
}

void NoteLane::ToggleDebugOverlay()
{
    m_renderer->ToggleDebugOverlay();
}
