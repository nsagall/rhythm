#include "NoteLane.h"

#include <cmath>
#include <string>
#include <vector>

namespace
{

constexpr UINT_PTR kFrameTimerId = 400;
constexpr UINT kFrameIntervalMs = 16;
constexpr int kDotRadius = 7;
constexpr DWORD kFlashDurationMs = 150;

// How much of the pattern timeline is visible across the lane: onsets up to
// kBeatsAhead beats in the future appear at the right edge, and onsets up
// to kBeatsBehind beats in the past remain visible before scrolling off the
// left edge. The judge line sits at beatsFromNow == 0.
constexpr double kBeatsAhead = 3.0;
constexpr double kBeatsBehind = 1.0;

// Returns every pattern onset (absolute beat position) within [fromBeat, toBeat].
std::vector<double> OnsetsInRange(double fromBeat, double toBeat, const ChartInstrument& instrument)
{
    std::vector<double> onsets;
    if (instrument.patternBeats.empty() || instrument.spanBeats <= 0.0)
    {
        return onsets;
    }

    long long firstBar = static_cast<long long>(std::floor(fromBeat / instrument.spanBeats)) - 1;
    long long lastBar = static_cast<long long>(std::floor(toBeat / instrument.spanBeats)) + 1;

    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (double onset : instrument.patternBeats)
        {
            double absolute = bar * instrument.spanBeats + onset;
            if (absolute >= fromBeat && absolute <= toBeat)
            {
                onsets.push_back(absolute);
            }
        }
    }
    return onsets;
}

} // namespace

// Stores the window that owns this lane's timer and repaints.
void NoteLane::Attach(HWND hwnd)
{
    m_hwnd = hwnd;
}

// Sets the screen-space rect the lane is drawn and animated within.
void NoteLane::SetLaneRect(RECT rect)
{
    m_laneRect = rect;
}

// Starts the redraw timer.
void NoteLane::StartAnimating()
{
    SetTimer(m_hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);
}

// Stops the redraw timer.
void NoteLane::StopAnimating()
{
    KillTimer(m_hwnd, kFrameTimerId);
}

// Handles a WM_TIMER tick; returns true if it belonged to this lane.
bool NoteLane::OnTimer(WPARAM timerId)
{
    if (timerId != kFrameTimerId)
    {
        return false;
    }
    InvalidateRect(m_hwnd, &m_laneRect, FALSE);
    return true;
}

// Flashes a brief hit/miss indicator at the judge line.
void NoteLane::ShowJudgement(JudgementResult result)
{
    m_flashResult = result;
    m_flashUntilMs = GetTickCount() + kFlashDurationMs;
}

// Paints the lane, judge line, upcoming onset dots, and status text for the given session.
void NoteLane::Draw(HDC hdc, const GameSession& session) const
{
    HBRUSH laneBrush = CreateSolidBrush(RGB(245, 245, 245));
    FillRect(hdc, &m_laneRect, laneBrush);
    DeleteObject(laneBrush);
    FrameRect(hdc, &m_laneRect, (HBRUSH)GetStockObject(BLACK_BRUSH));

    double laneWidth = m_laneRect.right - m_laneRect.left;
    double pixelsPerBeat = laneWidth / (kBeatsAhead + kBeatsBehind);
    int lineX = m_laneRect.left + static_cast<int>(kBeatsBehind * pixelsPerBeat);

    COLORREF lineColor = RGB(200, 0, 0);
    if (GetTickCount() < m_flashUntilMs)
    {
        lineColor = (m_flashResult == JudgementResult::Hit) ? RGB(0, 170, 0) : RGB(220, 0, 0);
    }
    HPEN linePen = CreatePen(PS_SOLID, 3, lineColor);
    HPEN oldPen = (HPEN)SelectObject(hdc, linePen);
    MoveToEx(hdc, lineX, m_laneRect.top, nullptr);
    LineTo(hdc, lineX, m_laneRect.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    const ChartInstrument* instrument = session.CurrentInstrument();
    bool showDots = instrument && session.Phase() == GamePhase::Learning;
    if (showDots)
    {
        double nowBeat = session.Clock().BeatPosition();
        std::vector<double> onsets = OnsetsInRange(nowBeat - kBeatsBehind, nowBeat + kBeatsAhead, *instrument);

        int centerY = (m_laneRect.top + m_laneRect.bottom) / 2;

        for (double onsetBeat : onsets)
        {
            double beatsFromNow = onsetBeat - nowBeat;
            int x = m_laneRect.left + static_cast<int>((beatsFromNow + kBeatsBehind) * pixelsPerBeat);
            if (x < m_laneRect.left - kDotRadius || x > m_laneRect.right + kDotRadius)
            {
                continue;
            }

            // Upcoming dots (not yet at the line) stay the default color; once a
            // dot has passed the line, it takes on the color of how that onset
            // was judged (green for a hit, red for a miss).
            COLORREF dotColor = RGB(0, 100, 220);
            if (beatsFromNow <= 0.0)
            {
                JudgementResult result = session.OnsetJudgement(onsetBeat);
                if (result == JudgementResult::Hit)
                {
                    dotColor = RGB(0, 170, 0);
                }
                else if (result == JudgementResult::Miss)
                {
                    dotColor = RGB(220, 0, 0);
                }
            }

            HBRUSH dotBrush = CreateSolidBrush(dotColor);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, dotBrush);
            Ellipse(hdc, x - kDotRadius, centerY - kDotRadius, x + kDotRadius, centerY + kDotRadius);
            SelectObject(hdc, oldBrush);
            DeleteObject(dotBrush);
        }
    }

    std::wstring status;
    switch (session.Phase())
    {
        case GamePhase::Idle:
            status = L"Load a chart and press Start";
            break;
        case GamePhase::CountIn:
            status = L"Get ready...";
            break;
        case GamePhase::Learning:
            if (instrument)
            {
                status = instrument->name + L"   " + std::to_wstring(session.CurrentStreak()) + L"/" +
                         std::to_wstring(instrument->hitsRequired);
            }
            break;
        case GamePhase::Complete:
            status = L"Song complete!";
            break;
    }

    RECT textRect = m_laneRect;
    textRect.left += 6;
    textRect.top += 4;
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, status.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
}
