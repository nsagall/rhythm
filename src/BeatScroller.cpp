#include "BeatScroller.h"

#include <algorithm>

namespace {

constexpr UINT_PTR kSpawnTimerId = 300;
constexpr UINT_PTR kFrameTimerId = 301;
constexpr UINT kFrameIntervalMs = 16;
constexpr int kDotRadius = 7;
// The vertical line sits this far into the lane from the left edge, giving
// dots room to "approach" from the right before they cross it.
constexpr double kLineFraction = 0.25;

} // namespace

void BeatScroller::Attach(HWND hwnd) {
    m_hwnd = hwnd;
}

void BeatScroller::SetLaneRect(RECT rect) {
    m_laneRect = rect;
}

int BeatScroller::GetLineX() const {
    int width = m_laneRect.right - m_laneRect.left;
    return m_laneRect.left + static_cast<int>(width * kLineFraction);
}

void BeatScroller::Start(int bpm) {
    bpm = std::max(bpm, 1);
    double beatPeriodMs = 60000.0 / bpm;

    // Dots are spawned once per beat period and travel at a constant speed
    // chosen so each one reaches the line exactly one beat period after
    // spawning - so consecutive dots cross the line at the beat rate.
    int travelToLine = m_laneRect.right - GetLineX();
    m_speedPxPerMs = travelToLine / beatPeriodMs;

    int laneWidth = m_laneRect.right - m_laneRect.left;
    m_dotLifetimeMs = m_speedPxPerMs > 0.0 ? static_cast<DWORD>(laneWidth / m_speedPxPerMs) : 0;

    m_dotSpawnTimes.clear();
    m_isPlaying = true;

    SetTimer(m_hwnd, kSpawnTimerId, static_cast<UINT>(beatPeriodMs), nullptr);
    SetTimer(m_hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);

    SpawnDot();
    InvalidateRect(m_hwnd, &m_laneRect, FALSE);
}

void BeatScroller::Stop() {
    KillTimer(m_hwnd, kSpawnTimerId);
    KillTimer(m_hwnd, kFrameTimerId);
    m_isPlaying = false;
    m_dotSpawnTimes.clear();
    InvalidateRect(m_hwnd, &m_laneRect, FALSE);
}

void BeatScroller::SpawnDot() {
    m_dotSpawnTimes.push_back(GetTickCount());
}

bool BeatScroller::OnTimer(WPARAM timerId) {
    if (timerId == kSpawnTimerId) {
        SpawnDot();
        return true;
    }

    if (timerId == kFrameTimerId) {
        DWORD now = GetTickCount();
        DWORD lifetime = m_dotLifetimeMs;
        m_dotSpawnTimes.erase(
            std::remove_if(m_dotSpawnTimes.begin(), m_dotSpawnTimes.end(),
                            [now, lifetime](DWORD spawnTime) { return now - spawnTime > lifetime; }),
            m_dotSpawnTimes.end());
        InvalidateRect(m_hwnd, &m_laneRect, FALSE);
        return true;
    }

    return false;
}

void BeatScroller::Draw(HDC hdc) const {
    HBRUSH laneBrush = CreateSolidBrush(RGB(245, 245, 245));
    FillRect(hdc, &m_laneRect, laneBrush);
    DeleteObject(laneBrush);
    FrameRect(hdc, &m_laneRect, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HPEN linePen = CreatePen(PS_SOLID, 2, RGB(200, 0, 0));
    HPEN oldPen = (HPEN)SelectObject(hdc, linePen);
    int lineX = GetLineX();
    MoveToEx(hdc, lineX, m_laneRect.top, nullptr);
    LineTo(hdc, lineX, m_laneRect.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    if (m_isPlaying) {
        DWORD now = GetTickCount();
        int centerY = (m_laneRect.top + m_laneRect.bottom) / 2;

        HBRUSH dotBrush = CreateSolidBrush(RGB(0, 100, 220));
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, dotBrush);

        for (DWORD spawnTime : m_dotSpawnTimes) {
            DWORD elapsed = now - spawnTime;
            int x = m_laneRect.right - static_cast<int>(m_speedPxPerMs * elapsed);
            if (x < m_laneRect.left - kDotRadius || x > m_laneRect.right + kDotRadius) {
                continue;
            }
            Ellipse(hdc, x - kDotRadius, centerY - kDotRadius, x + kDotRadius, centerY + kDotRadius);
        }

        SelectObject(hdc, oldBrush);
        DeleteObject(dotBrush);
    }
}
