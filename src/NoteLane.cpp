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
    InvalidateRect(m_hwnd, &m_laneRect, FALSE);
    return true;
}

void NoteLane::ShowJudgement(JudgementResult result, int lane, bool lockedIn)
{
    m_renderer->OnJudgement(result, lane, lockedIn);
}

void NoteLane::Draw(HDC hdc, const GameSession& session)
{
    NoteLaneScene scene = m_model.BuildScene(session);
    m_renderer->Draw(hdc, m_laneRect, scene);
}
