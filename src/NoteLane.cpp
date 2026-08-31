#include "NoteLane.h"

#include "NoteLaneGdiRenderer.h"

namespace
{
constexpr UINT_PTR c_FrameTimerId = 400;
constexpr UINT c_FrameIntervalMs = 16;
} // namespace

// Defaults to the GDI renderer, the only concrete INoteLaneRenderer today. Only this constructor
// knows the concrete type; the rest of the class uses m_renderer through the interface.
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
    SetTimer(m_hwnd, c_FrameTimerId, c_FrameIntervalMs, nullptr);
}

void NoteLane::StopAnimating()
{
    KillTimer(m_hwnd, c_FrameTimerId);
}

bool NoteLane::OnTimer(WPARAM timerId)
{
    if (timerId != c_FrameTimerId)
    {
        return false;
    }
    // The hits meter panel sits outside m_laneRect, so invalidate both rects every animating frame.
    RECT dirtyRect{};
    UnionRect(&dirtyRect, &m_laneRect, &m_hitsMeterRect);
    InvalidateRect(m_hwnd, &dirtyRect, FALSE);
    return true;
}

void NoteLane::ShowJudgement(JudgementResult result, int lane, bool passing, bool precise)
{
    m_renderer->OnJudgement(result, lane, passing, precise);
}

void NoteLane::ShowHudValueChanged(GameSession::HudField field, int newValue)
{
    m_renderer->OnHudValueChanged(field, newValue);
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
