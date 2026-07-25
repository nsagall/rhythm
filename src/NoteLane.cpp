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

// The rhythmic note values a glyph can be drawn as.
enum class NoteDuration
{
    Whole,
    Half,
    Quarter,
    Eighth,
    Sixteenth,
};

// Buckets a gap-to-next-onset (in beats) into the nearest note value, using
// geometric midpoints between the standard durations (4, 2, 1, 0.5, 0.25).
NoteDuration ClassifyDuration(double beats)
{
    if (beats >= 2.828) return NoteDuration::Whole;
    if (beats >= 1.414) return NoteDuration::Half;
    if (beats >= 0.707) return NoteDuration::Quarter;
    if (beats >= 0.354) return NoteDuration::Eighth;
    return NoteDuration::Sixteenth;
}

// Returns how many beats separate this onset from the next one in the
// instrument's repeating pattern (wrapping around the span), which is what
// determines its drawn note value - a pattern's onset spacing stands in for
// an explicit duration field, which the chart format doesn't have.
double DurationBeatsForOnset(double onsetBeat, const ChartInstrument& instrument)
{
    if (instrument.patternBeats.size() < 2 || instrument.spanBeats <= 0.0)
    {
        return instrument.spanBeats > 0.0 ? instrument.spanBeats : 1.0;
    }

    double phase = std::fmod(onsetBeat, instrument.spanBeats);
    if (phase < 0.0)
    {
        phase += instrument.spanBeats;
    }

    size_t closestIndex = 0;
    double closestDiff = std::abs(instrument.patternBeats[0] - phase);
    for (size_t i = 1; i < instrument.patternBeats.size(); ++i)
    {
        double diff = std::abs(instrument.patternBeats[i] - phase);
        if (diff < closestDiff)
        {
            closestDiff = diff;
            closestIndex = i;
        }
    }

    double next = (closestIndex + 1 < instrument.patternBeats.size())
                      ? instrument.patternBeats[closestIndex + 1]
                      : instrument.patternBeats[0] + instrument.spanBeats;
    return next - instrument.patternBeats[closestIndex];
}

// Draws a music-notation-style note glyph (notehead, stem, and flags for
// eighth/sixteenth notes) centered at (x, centerY), entirely in color, so it
// still reads as "hit" green / "miss" red / default blue exactly like the
// plain dot it replaces.
void DrawNoteGlyph(HDC hdc, int x, int centerY, NoteDuration duration, COLORREF color)
{
    constexpr int kHeadRadiusX = 7;
    constexpr int kHeadRadiusY = 6;
    constexpr int kStemHeight = 22;
    constexpr int kStemThickness = 2;

    HPEN pen = CreatePen(PS_SOLID, kStemThickness, color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH filledBrush = CreateSolidBrush(color);
    HBRUSH hollowBrush = (HBRUSH)GetStockObject(NULL_BRUSH);

    bool isHollow = (duration == NoteDuration::Whole || duration == NoteDuration::Half);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, isHollow ? hollowBrush : filledBrush);
    Ellipse(hdc, x - kHeadRadiusX, centerY - kHeadRadiusY, x + kHeadRadiusX, centerY + kHeadRadiusY);
    SelectObject(hdc, oldBrush);

    if (duration != NoteDuration::Whole)
    {
        int stemX = x + kHeadRadiusX - 1;
        int stemTopY = centerY - kStemHeight;
        MoveToEx(hdc, stemX, centerY, nullptr);
        LineTo(hdc, stemX, stemTopY);

        int flagCount = (duration == NoteDuration::Eighth) ? 1 : (duration == NoteDuration::Sixteenth ? 2 : 0);
        HBRUSH oldFlagBrush = (HBRUSH)SelectObject(hdc, filledBrush);
        for (int flag = 0; flag < flagCount; ++flag)
        {
            int flagY = stemTopY + flag * 6;
            POINT flagPoints[3] = {
                {stemX, flagY},
                {stemX + 8, flagY + 5},
                {stemX + 1, flagY + 11},
            };
            Polygon(hdc, flagPoints, 3);
        }
        SelectObject(hdc, oldFlagBrush);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    DeleteObject(filledBrush);
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
    bool isLiveJudging = instrument && session.Phase() == GamePhase::Learning &&
                          !session.IsAwaitingAdvance() && !session.IsInIntro();

    // While the player can't act yet (count-in, an instrument's intro, or
    // the wait before the next instrument joins), start showing the
    // upcoming instrument's dots from its actual first required onset
    // onward - so each one scrolls in from the right edge one at a time,
    // exactly like live play, instead of a whole batch of already-
    // partially-scrolled-in dots popping in together once revealed.
    const ChartInstrument* dotsInstrument = nullptr;
    double dotsFromBeat = 0.0;
    double nowBeat = session.Clock().BeatPosition();

    if (isLiveJudging)
    {
        dotsInstrument = instrument;
        dotsFromBeat = nowBeat - kBeatsBehind;
    }
    else
    {
        const ChartInstrument* preview = session.PreviewInstrument();
        double firstOnsetBeat = session.PreviewFirstOnsetBeat();
        if (preview && firstOnsetBeat >= 0.0)
        {
            dotsInstrument = preview;
            dotsFromBeat = firstOnsetBeat;
        }
    }

    if (dotsInstrument)
    {
        std::vector<double> onsets = OnsetsInRange(dotsFromBeat, nowBeat + kBeatsAhead, *dotsInstrument);

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

            NoteDuration duration = ClassifyDuration(DurationBeatsForOnset(onsetBeat, *dotsInstrument));
            DrawNoteGlyph(hdc, x, centerY, duration, dotColor);
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
                if (session.IsInIntro())
                {
                    status = instrument->name + L" — Listen...";
                }
                else
                {
                    status = instrument->name + L"   " + std::to_wstring(session.CurrentStreak()) + L"/" +
                             std::to_wstring(instrument->hitsRequired);
                }
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
