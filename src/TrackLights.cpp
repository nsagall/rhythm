#include "TrackLights.h"

#include <algorithm>

namespace {

constexpr int kTimerIdBase = 200;

bool IsOurTimerId(WPARAM timerId, int& outTrackIndex) {
    int id = static_cast<int>(timerId);
    if (id < kTimerIdBase || id >= kTimerIdBase + kTrackCount) {
        return false;
    }
    outTrackIndex = id - kTimerIdBase;
    return true;
}

} // namespace

void TrackLights::Attach(HWND hwnd) {
    m_hwnd = hwnd;
}

void TrackLights::SetLightRect(int trackIndex, RECT rect) {
    m_lightRects[trackIndex] = rect;
}

void TrackLights::Start(int trackIndex, int bpm) {
    bpm = std::max(bpm, 1);
    int intervalMs = 30000 / bpm;

    m_lightOn[trackIndex] = true;
    SetTimer(m_hwnd, kTimerIdBase + trackIndex, intervalMs, nullptr);
    InvalidateRect(m_hwnd, &m_lightRects[trackIndex], FALSE);
}

void TrackLights::Stop(int trackIndex) {
    KillTimer(m_hwnd, kTimerIdBase + trackIndex);
    m_lightOn[trackIndex] = false;
    InvalidateRect(m_hwnd, &m_lightRects[trackIndex], FALSE);
}

void TrackLights::StopAll() {
    for (int i = 0; i < kTrackCount; ++i) {
        Stop(i);
    }
}

bool TrackLights::OnTimer(WPARAM timerId) {
    int trackIndex = 0;
    if (!IsOurTimerId(timerId, trackIndex)) {
        return false;
    }

    m_lightOn[trackIndex] = !m_lightOn[trackIndex];
    InvalidateRect(m_hwnd, &m_lightRects[trackIndex], FALSE);
    return true;
}

void TrackLights::Draw(HDC hdc) const {
    HBRUSH onBrush = CreateSolidBrush(RGB(0, 200, 0));
    HBRUSH offBrush = CreateSolidBrush(RGB(60, 60, 60));

    for (int i = 0; i < kTrackCount; ++i) {
        const RECT& r = m_lightRects[i];
        HBRUSH brush = m_lightOn[i] ? onBrush : offBrush;
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
        Ellipse(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oldBrush);
    }

    DeleteObject(onBrush);
    DeleteObject(offBrush);
}
