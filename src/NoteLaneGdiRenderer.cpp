#include "NoteLaneGdiRenderer.h"

#include <algorithm>
#include <cmath>

#include "ClipColor.h"
#include "ColorUtil.h"

using ColorUtil::ClampChannel;
using ColorUtil::Darken;
using ColorUtil::Lighten;

namespace
{

constexpr int kVisibilityMarginPx = 12;
constexpr DWORD kFlashDurationMs = 220;
constexpr DWORD kConfettiDurationMs = 1400;
constexpr int kConfettiPieceCount = 60;

// Notes-exploding burst - shorter and snappier than the confetti burst
// since it's marking a bunch of individual notes popping at once, not one
// big celebration.
constexpr DWORD kExplosionDurationMs = 500;
constexpr int kExplosionParticlesPerNote = 10;
constexpr double kExplosionMinSpeedPxPerSec = 90.0;
constexpr double kExplosionMaxSpeedPxPerSec = 260.0;
constexpr double kExplosionDragPerSec = 2.5; // exponential slowdown - a burst outward that settles, not a straight-line fly-off

// The hits meter's own little "lock-in" burst - a single celebration point
// rather than one burst per note, so it gets a bit more than one note's
// worth of sparks (kExplosionParticlesPerNote) to still read as an event
// worth noticing beside the (possibly much larger) note-explosion burst
// firing at the very same instant.
constexpr int kHitsMeterExplosionParticleCount = 16;

// The hits meter's own confetti burst (Pass mode only - see
// AppendHitsMeterConfetti) - fewer pieces than the full-width playfield
// burst (kConfettiPieceCount), scaled down to match the meter's own much
// narrower width.
constexpr int kHitsMeterConfettiPieceCount = 18;

// Hit ripples grow twice as fast as miss ripples, which are themselves a
// bit faster than a plain 260 px/sec so they fully fade before leaving the lane.
constexpr double kMissRippleSpeedPxPerSec = 260.0 / 0.75;
constexpr double kHitRippleSpeedPxPerSec = kMissRippleSpeedPxPerSec / 0.5;
constexpr int kRippleThicknessPx = 4;

// Starting opacity for each ripple color - both fade further from here as
// they grow, never brighter.
constexpr int kHitRippleStartAlpha = static_cast<int>(255 * 0.5);   // 50% transparent to start
constexpr int kMissRippleStartAlpha = static_cast<int>(255 * 0.25); // 75% transparent to start

// A hit ripple's alpha reaches zero at half the radius (so half the time)
// a miss ripple's does - it fades away twice as fast, independent of how
// fast either one is still visibly growing/traveling toward off-screen.
constexpr double kHitRippleFadeRate = 2.0;
constexpr double kMissRippleFadeRate = 1.0;

constexpr int kColumnGutterPx = 4;
constexpr double kBarWidthFraction = 0.48;
constexpr int kGlyphRadiusX = 7;
constexpr int kGlyphRadiusY = 7;
constexpr int kBarCornerRadius = 11;
constexpr int kReceptorRadius = 14;

// Deep indigo-to-violet backdrop - moody enough to make bright note colors
// pop, warm enough to not read as a cold "dev tool" dark mode.
constexpr COLORREF kBgTop = RGB(46, 20, 92);
constexpr COLORREF kBgBottom = RGB(15, 11, 42);
constexpr COLORREF kBorderColor = RGB(120, 90, 190);
constexpr COLORREF kTextColor = RGB(255, 250, 240);
constexpr COLORREF kStreakColor = RGB(255, 205, 70); // still used as one of the confetti burst's colors

// Pass/fail feedback: saturated, high-contrast green/red, pushed toward
// each color's most vivid extreme (rather than a softer pastel) so the
// judgement reads instantly at a glance - paired with a glow halo so it
// pops rather than just being a flat color swap. kNoteColorHit doubles as
// the "correctly held, not yet released" color.
constexpr COLORREF kNoteColorHit = RGB(40, 235, 80);
constexpr COLORREF kNoteColorMiss = RGB(255, 30, 30);

// A hit whose press wasn't precise (see GameSession::JudgementEvent::precise)
// still ripples - it was a correct press - but yellow instead of green, so
// "correct but sloppy" reads differently at a glance from "correct and
// tight." Only the ripple uses this; the note/receptor/flash colors stay
// kNoteColorHit regardless, since those already existed before accuracy was
// tracked and this is deliberately a smaller, additive cue on top.
constexpr COLORREF kNoteColorHitImprecise = RGB(255, 205, 40);

// Palette confetti pieces are drawn from at lock-in - a small fixed set of
// "party colors" of its own, deliberately independent of any clip's color
// (this is a generic celebration, not an identity cue).
constexpr COLORREF kConfettiPalette[] = {
    RGB(255, 70, 235), RGB(56, 219, 255), RGB(255, 205, 70), RGB(190, 140, 255), kStreakColor, RGB(255, 255, 255),
};
constexpr int kConfettiPaletteSize = sizeof(kConfettiPalette) / sizeof(kConfettiPalette[0]);

// Cheap, stable pseudo-random float in [0,1) derived from an integer seed - used to scatter
// background sparkles/confetti/explosion particles at fixed-looking-random positions without
// needing <random> state.
double PseudoRandom(int seed)
{
    unsigned int x = static_cast<unsigned int>(seed) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return (x % 10000) / 10000.0;
}

// Fills [left,top,right,bottom) with a smooth vertical gradient from topColor to bottomColor.
void FillVerticalGradient(HDC hdc, RECT rect, COLORREF topColor, COLORREF bottomColor)
{
    TRIVERTEX vertices[2];
    vertices[0].x = rect.left;
    vertices[0].y = rect.top;
    vertices[0].Red = static_cast<COLOR16>(GetRValue(topColor) << 8);
    vertices[0].Green = static_cast<COLOR16>(GetGValue(topColor) << 8);
    vertices[0].Blue = static_cast<COLOR16>(GetBValue(topColor) << 8);
    vertices[0].Alpha = 0;
    vertices[1].x = rect.right;
    vertices[1].y = rect.bottom;
    vertices[1].Red = static_cast<COLOR16>(GetRValue(bottomColor) << 8);
    vertices[1].Green = static_cast<COLOR16>(GetGValue(bottomColor) << 8);
    vertices[1].Blue = static_cast<COLOR16>(GetBValue(bottomColor) << 8);
    vertices[1].Alpha = 0;

    GRADIENT_RECT gradientRect{0, 1};
    GradientFill(hdc, vertices, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V);
}

} // namespace

NoteLaneGdiRenderer::AlphaShape::AlphaShape(HDC destHdc, RECT bounds, HDC scratchDc)
    : m_destHdc(destHdc), m_bounds(bounds)
{
    m_width = bounds.right - bounds.left;
    m_height = bounds.bottom - bounds.top;
    if (m_width <= 0 || m_height <= 0)
    {
        return;
    }
    m_memHdc = scratchDc;
    BitBlt(m_memHdc, 0, 0, m_width, m_height, destHdc, bounds.left, bounds.top, SRCCOPY);
}

// Blends the captured+drawn-on scratch bitmap back onto the destination at the given alpha (0-255).
void NoteLaneGdiRenderer::AlphaShape::Blend(BYTE alpha) const
{
    if (!m_memHdc || alpha == 0)
    {
        return;
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(m_destHdc, m_bounds.left, m_bounds.top, m_width, m_height, m_memHdc, 0, 0, m_width, m_height, blend);
}

NoteLaneGdiRenderer::~NoteLaneGdiRenderer()
{
    for (auto& entry : m_brushCache)
    {
        DeleteObject(entry.second);
    }
    for (auto& entry : m_penCache)
    {
        DeleteObject(entry.second);
    }
    if (m_scratchBitmap)
    {
        SelectObject(m_scratchDc, m_scratchDefaultBitmap);
        DeleteObject(m_scratchBitmap);
    }
    if (m_scratchDc)
    {
        DeleteDC(m_scratchDc);
    }
    if (m_hudFont)
    {
        DeleteObject(m_hudFont);
    }
}

double NoteLaneGdiRenderer::PixelsPerBeat(RECT laneRect) const
{
    double laneHeight = laneRect.bottom - laneRect.top;
    return laneHeight / (kBeatsAhead + kBeatsBehind);
}

int NoteLaneGdiRenderer::LineY(RECT laneRect) const
{
    return laneRect.top + static_cast<int>(kBeatsAhead * PixelsPerBeat(laneRect));
}

// Notes spawn at the top (the future) and scroll straight down toward the
// judge line near the bottom: kBeatsAhead of the timeline occupies the
// space above the line, kBeatsBehind the space below it. Shared across
// every column - only the horizontal position varies by lane.
int NoteLaneGdiRenderer::LaneCenterX(RECT laneRect, int lane) const
{
    double totalWidth = laneRect.right - laneRect.left;
    double columnWidth = (totalWidth - kColumnGutterPx * (kLaneCount - 1)) / kLaneCount;
    return laneRect.left + static_cast<int>(lane * (columnWidth + kColumnGutterPx) + columnWidth / 2.0);
}

int NoteLaneGdiRenderer::YForBeatsFromNow(RECT laneRect, double beatsFromNow) const
{
    return laneRect.top + static_cast<int>((kBeatsAhead - beatsFromNow) * PixelsPerBeat(laneRect));
}

COLORREF NoteLaneGdiRenderer::ColorForNote(NoteVisualState state, COLORREF clipColor) const
{
    switch (state)
    {
        case NoteVisualState::Held:
        case NoteVisualState::Hit:
            return kNoteColorHit;
        case NoteVisualState::Miss:
            return kNoteColorMiss;
        case NoteVisualState::Normal:
        default:
            return clipColor;
    }
}

// Returns a solid brush for `color`, creating it once and reusing it for
// this renderer's lifetime - every note/receptor/ripple/confetti piece
// redraws every single frame (up to 60 times a second) from a small,
// bounded palette (clip colors, hit/miss, and their Darken()/
// Lighten() derivatives), so creating and immediately destroying a fresh
// HBRUSH for each one would be pure per-frame GDI churn for no benefit.
HBRUSH NoteLaneGdiRenderer::CachedSolidBrush(COLORREF color)
{
    HBRUSH& brush = m_brushCache[color];
    if (!brush)
    {
        brush = CreateSolidBrush(color);
    }
    return brush;
}

// Same idea as CachedSolidBrush, for the PS_SOLID pens used to outline
// rings and the lane border - (thickness, color) pairs are the same
// small, bounded set every frame.
HPEN NoteLaneGdiRenderer::CachedSolidPen(int width, COLORREF color)
{
    UINT64 key = (static_cast<UINT64>(static_cast<UINT32>(width)) << 32) | static_cast<UINT32>(color);
    HPEN& pen = m_penCache[key];
    if (!pen)
    {
        pen = CreatePen(PS_SOLID, width, color);
    }
    return pen;
}

HDC NoteLaneGdiRenderer::EnsureScratchBuffer(HDC referenceHdc, int width, int height)
{
    if (!m_scratchDc)
    {
        m_scratchDc = CreateCompatibleDC(referenceHdc);
    }
    if (width > m_scratchWidth || height > m_scratchHeight)
    {
        if (m_scratchBitmap)
        {
            SelectObject(m_scratchDc, m_scratchDefaultBitmap);
            DeleteObject(m_scratchBitmap);
        }
        m_scratchWidth = std::max(width, m_scratchWidth);
        m_scratchHeight = std::max(height, m_scratchHeight);
        m_scratchBitmap = CreateCompatibleBitmap(referenceHdc, m_scratchWidth, m_scratchHeight);
        HBITMAP previous = (HBITMAP)SelectObject(m_scratchDc, m_scratchBitmap);
        if (!m_scratchDefaultBitmap)
        {
            m_scratchDefaultBitmap = previous;
        }
    }
    return m_scratchDc;
}

// Alpha-blends a filled circle centered at (cx, cy) with the given radius/color/alpha.
void NoteLaneGdiRenderer::DrawAlphaCircle(HDC hdc, int cx, int cy, int radius, COLORREF color, BYTE alpha)
{
    if (radius <= 0)
    {
        return;
    }
    RECT bounds{cx - radius, cy - radius, cx + radius, cy + radius};
    AlphaShape shape(hdc, bounds, EnsureScratchBuffer(hdc, bounds.right - bounds.left, bounds.bottom - bounds.top));
    if (!shape.Dc())
    {
        return;
    }
    HBRUSH oldBrush = (HBRUSH)SelectObject(shape.Dc(), CachedSolidBrush(color));
    HPEN oldPen = (HPEN)SelectObject(shape.Dc(), GetStockObject(NULL_PEN));
    Ellipse(shape.Dc(), 0, 0, radius * 2, radius * 2);
    SelectObject(shape.Dc(), oldBrush);
    SelectObject(shape.Dc(), oldPen);
    shape.Blend(alpha);
}

// Alpha-blends an unfilled ring (circle outline) centered at (cx, cy).
void NoteLaneGdiRenderer::DrawAlphaRing(HDC hdc, int cx, int cy, int radius, int thickness, COLORREF color,
                                         BYTE alpha)
{
    if (radius <= 0)
    {
        return;
    }
    RECT bounds{cx - radius, cy - radius, cx + radius, cy + radius};
    AlphaShape shape(hdc, bounds, EnsureScratchBuffer(hdc, bounds.right - bounds.left, bounds.bottom - bounds.top));
    if (!shape.Dc())
    {
        return;
    }
    HPEN oldPen = (HPEN)SelectObject(shape.Dc(), CachedSolidPen(thickness, color));
    HBRUSH oldBrush = (HBRUSH)SelectObject(shape.Dc(), GetStockObject(NULL_BRUSH));
    Ellipse(shape.Dc(), thickness, thickness, radius * 2 - thickness, radius * 2 - thickness);
    SelectObject(shape.Dc(), oldBrush);
    SelectObject(shape.Dc(), oldPen);
    shape.Blend(alpha);
}

// Alpha-blends a filled rounded rectangle - used for rails and the translucent HUD panel.
void NoteLaneGdiRenderer::DrawAlphaRoundRect(HDC hdc, RECT rect, int cornerRadius, COLORREF color, BYTE alpha)
{
    AlphaShape shape(hdc, rect, EnsureScratchBuffer(hdc, rect.right - rect.left, rect.bottom - rect.top));
    if (!shape.Dc())
    {
        return;
    }
    HBRUSH oldBrush = (HBRUSH)SelectObject(shape.Dc(), CachedSolidBrush(color));
    HPEN oldPen = (HPEN)SelectObject(shape.Dc(), GetStockObject(NULL_PEN));
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    RoundRect(shape.Dc(), 0, 0, w, h, cornerRadius, cornerRadius);
    SelectObject(shape.Dc(), oldBrush);
    SelectObject(shape.Dc(), oldPen);
    shape.Blend(alpha);
}

// Alpha-blends a filled rectangle centered at (cx, cy) with the given
// half-width/half-height/color/alpha - used for confetti pieces and measure lines.
void NoteLaneGdiRenderer::DrawAlphaRect(HDC hdc, int cx, int cy, int halfWidth, int halfHeight, COLORREF color,
                                         BYTE alpha)
{
    if (halfWidth <= 0 || halfHeight <= 0)
    {
        return;
    }
    RECT bounds{cx - halfWidth, cy - halfHeight, cx + halfWidth, cy + halfHeight};
    AlphaShape shape(hdc, bounds, EnsureScratchBuffer(hdc, bounds.right - bounds.left, bounds.bottom - bounds.top));
    if (!shape.Dc())
    {
        return;
    }
    RECT local{0, 0, halfWidth * 2, halfHeight * 2};
    FillRect(shape.Dc(), &local, CachedSolidBrush(color));
    shape.Blend(alpha);
}

// Draws a music-notation-free notehead marker: dark bevel rim, color fill, a soft alpha glow
// behind it, and a small specular highlight - the note's start marker, sitting at the bottom
// (leading) edge of its duration bar. passing adds a second, wider green glow ring underneath
// everything else - a supplementary "this track is currently passing" cue that doesn't touch
// color.
void NoteLaneGdiRenderer::DrawNoteGlyph(HDC hdc, int x, int y, COLORREF color, bool glow, bool passing)
{
    if (passing)
    {
        DrawAlphaCircle(hdc, x, y, kGlyphRadiusX + 14, kNoteColorHit, 90);
    }

    if (glow)
    {
        DrawAlphaCircle(hdc, x, y, kGlyphRadiusX + 10, color, 60);
    }

    // Soft drop shadow for a little lift off the background.
    DrawAlphaCircle(hdc, x + 2, y + 3, kGlyphRadiusX, RGB(0, 0, 0), 70);

    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HPEN oldPen = (HPEN)SelectObject(hdc, nullPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    SelectObject(hdc, CachedSolidBrush(Darken(color, 100)));
    Ellipse(hdc, x - kGlyphRadiusX, y - kGlyphRadiusY, x + kGlyphRadiusX, y + kGlyphRadiusY);

    SelectObject(hdc, CachedSolidBrush(color));
    Ellipse(hdc, x - kGlyphRadiusX + 2, y - kGlyphRadiusY + 2, x + kGlyphRadiusX - 1, y + kGlyphRadiusY - 1);

    SelectObject(hdc, CachedSolidBrush(Lighten(color, 150)));
    Ellipse(hdc, x - kGlyphRadiusX + 2, y - kGlyphRadiusY + 1, x - 1, y - 2);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}

// Draws a note's duration bar spanning [yTop, yBottom] at horizontal center
// x, with the same dark-rim/fill/highlight-strip bevel language as the
// glyph, so the bar and its start marker read as one consistent object.
// passing adds a glowing green outline around the whole bar underneath
// everything else, matching DrawNoteGlyph's own passing glow.
void NoteLaneGdiRenderer::DrawNoteBar(HDC hdc, int x, int yTop, int yBottom, int halfWidth, COLORREF color,
                                       bool passing)
{
    if (yBottom <= yTop)
    {
        return;
    }

    if (passing)
    {
        RECT glowRect{x - halfWidth - 5, yTop - 5, x + halfWidth + 5, yBottom + 5};
        DrawAlphaRoundRect(hdc, glowRect, kBarCornerRadius + 5, kNoteColorHit, 130);
    }

    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HPEN oldPen = (HPEN)SelectObject(hdc, nullPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    SelectObject(hdc, CachedSolidBrush(Darken(color, 100)));
    RoundRect(hdc, x - halfWidth, yTop, x + halfWidth, yBottom, kBarCornerRadius, kBarCornerRadius);

    SelectObject(hdc, CachedSolidBrush(color));
    RoundRect(hdc, x - halfWidth + 2, yTop + 2, x + halfWidth - 1, yBottom - 1, kBarCornerRadius, kBarCornerRadius);

    RECT highlightRect{x - halfWidth + 3, yTop + 5, x - halfWidth + 7, yBottom - 4};
    if (highlightRect.bottom > highlightRect.top)
    {
        FillRect(hdc, &highlightRect, CachedSolidBrush(Lighten(color, 100)));
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}

// Draws one lane's judge-line receptor: a glowing ring that sits dim by
// default, glows warmly while a note is held on it, and pops brightly for
// a moment when a press/release is judged.
void NoteLaneGdiRenderer::DrawReceptor(HDC hdc, int x, int y, COLORREF laneColor, bool held, bool flashing,
                                        COLORREF flashColor, double flashProgress)
{
    if (flashing)
    {
        double fade = 1.0 - flashProgress;
        DrawAlphaCircle(hdc, x, y, kReceptorRadius + static_cast<int>(14 * flashProgress), flashColor,
                         static_cast<BYTE>(160 * fade));
        DrawAlphaRing(hdc, x, y, kReceptorRadius, 4, Lighten(flashColor, 40), static_cast<BYTE>(255 * fade));
        return;
    }

    if (held)
    {
        DrawAlphaCircle(hdc, x, y, kReceptorRadius + 6, laneColor, 90);
        DrawAlphaRing(hdc, x, y, kReceptorRadius, 4, Lighten(laneColor, 60), 235);
        return;
    }

    DrawAlphaRing(hdc, x, y, kReceptorRadius, 3, laneColor, 130);
    DrawAlphaCircle(hdc, x, y, 3, Lighten(laneColor, 60), 150);
}

// A scattering of small twinkling sparkles for ambiance - drift gently on
// wall-clock time (alive even before the player presses Start) and get an
// extra brightness kick from the beat pulse once the song runs.
void NoteLaneGdiRenderer::DrawSparkles(HDC hdc, RECT laneRect, double beatPulse)
{
    DWORD ticks = GetTickCount();
    for (int i = 0; i < 14; ++i)
    {
        double fx = PseudoRandom(i * 3 + 1);
        double fy = PseudoRandom(i * 3 + 2);
        double phaseOffset = PseudoRandom(i * 3 + 3) * 6.283185307;
        double twinkle = 0.5 + 0.5 * std::sin(ticks / 900.0 + phaseOffset);
        int alpha = static_cast<int>(30 + twinkle * 70 + beatPulse * 60);
        int sx = laneRect.left + static_cast<int>(fx * (laneRect.right - laneRect.left));
        int sy = laneRect.top + static_cast<int>(fy * (laneRect.bottom - laneRect.top));
        int radius = (i % 3 == 0) ? 2 : 1;
        DrawAlphaCircle(hdc, sx, sy, radius, RGB(255, 255, 255), static_cast<BYTE>(ClampChannel(alpha)));
    }
}

// A faint horizontal line at every measure boundary crossing the visible
// window, so the falling pattern reads against the song's own bar grid
// instead of floating with no sense of scale.
void NoteLaneGdiRenderer::DrawMeasureLines(HDC hdc, RECT laneRect, const NoteLaneScene& scene)
{
    if (scene.beatsPerBar <= 0)
    {
        return;
    }
    double rangeStart = scene.nowBeat - kBeatsBehind;
    double rangeEnd = scene.nowBeat + kBeatsAhead;
    long long firstMeasure = static_cast<long long>(std::floor(rangeStart / scene.beatsPerBar));
    long long lastMeasure = static_cast<long long>(std::ceil(rangeEnd / scene.beatsPerBar));
    for (long long measure = firstMeasure; measure <= lastMeasure; ++measure)
    {
        double measureBeat = measure * scene.beatsPerBar;
        if (measureBeat < rangeStart || measureBeat > rangeEnd)
        {
            continue;
        }
        int y = YForBeatsFromNow(laneRect, measureBeat - scene.nowBeat);
        int cx = (laneRect.left + laneRect.right) / 2;
        int halfWidth = (laneRect.right - laneRect.left) / 2 - 10;
        DrawAlphaRect(hdc, cx, y, halfWidth, 1, RGB(255, 255, 255), 28);
    }
}

// A faint glowing rail down each column, in the current/upcoming clip's
// own color, so every lane has a visible identity even where no notes are
// currently on screen.
void NoteLaneGdiRenderer::DrawRails(HDC hdc, RECT laneRect, COLORREF primaryColor)
{
    int lineY = LineY(laneRect);
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        int x = LaneCenterX(laneRect, lane);
        RECT railRect{x - 2, laneRect.top + 10, x + 2, lineY};
        DrawAlphaRoundRect(hdc, railRect, 2, primaryColor, 26);
    }
}

// Judge-line receptors: one glowing ring per lane, dim by default, glowing
// while a note is held, and popping brightly for a moment when that
// lane's press/release was just judged (see OnJudgement).
void NoteLaneGdiRenderer::DrawReceptors(HDC hdc, RECT laneRect, const NoteLaneScene& scene, COLORREF primaryColor)
{
    int lineY = LineY(laneRect);
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        int x = LaneCenterX(laneRect, lane);
        bool held = scene.receptors[lane].held;
        bool flashing = GetTickCount() < m_flashUntilMs[lane];
        double flashProgress = 0.0;
        COLORREF flashColor = kNoteColorHit;
        if (flashing)
        {
            DWORD remaining = m_flashUntilMs[lane] - GetTickCount();
            flashProgress = 1.0 - (static_cast<double>(remaining) / kFlashDurationMs);
            flashColor = (m_flashResult[lane] == JudgementResult::Hit) ? kNoteColorHit : kNoteColorMiss;
        }
        DrawReceptor(hdc, x, lineY, primaryColor, held, flashing, flashColor, flashProgress);
    }
}

// Draws every note in scene.notes: upcoming notes in their own clip
// color; a held/hit note in kNoteColorHit; a missed note in kNoteColorMiss
// - both then hold for the rest of the note's time on screen, matching the
// real outcome rather than fading back to Normal. Judged hit/miss/held
// notes get the same glow halo as the passing outline (note.clip->passing)
// so the pass/fail moment pops instead of being a flat color swap.
void NoteLaneGdiRenderer::DrawNotes(HDC hdc, RECT laneRect, const NoteLaneScene& scene)
{
    double columnWidth =
        ((laneRect.right - laneRect.left) - kColumnGutterPx * (kLaneCount - 1)) / static_cast<double>(kLaneCount);
    int barHalfWidth = static_cast<int>(columnWidth * kBarWidthFraction / 2.0);

    for (const SceneNote& note : scene.notes)
    {
        int laneX = LaneCenterX(laneRect, note.lane);
        double beatsFromStart = note.startBeat - scene.nowBeat;
        double beatsFromEnd = beatsFromStart + note.durationBeats;
        int yBottom = YForBeatsFromNow(laneRect, beatsFromStart); // the start - the leading, lower edge
        int yTop = YForBeatsFromNow(laneRect, beatsFromEnd);      // the end - the trailing, upper edge
        if (yBottom < laneRect.top - kVisibilityMarginPx || yTop > laneRect.bottom + kVisibilityMarginPx)
        {
            continue;
        }

        COLORREF color = ColorForNote(note.state, note.clip->color);
        bool emphasisGlow = note.state != NoteVisualState::Normal;

        DrawNoteBar(hdc, laneX, yTop, yBottom, barHalfWidth, color, note.clip->passing);
        DrawNoteGlyph(hdc, laneX, yBottom, color, emphasisGlow, note.clip->passing);
    }
}

// Draws the in-progress lock-in confetti burst: each piece falls under
// simple gravity from its spawn position with a bit of horizontal drift,
// tumbling (a cheap width-scaling trick standing in for a spin, since GDI
// has no easy per-primitive rotation here) and fading out over its last
// stretch of life.
void NoteLaneGdiRenderer::DrawConfetti(HDC hdc, double elapsedSeconds, double t)
{
    constexpr double kGravityPxPerSec2 = 420.0;
    constexpr int kHalfHeightPx = 5;
    constexpr int kMaxHalfWidthPx = 4;

    BYTE fadeAlpha = static_cast<BYTE>(ClampChannel(static_cast<int>(255 * (1.0 - t * t))));

    for (const ConfettiPiece& piece : m_confetti)
    {
        int x = static_cast<int>(piece.x + piece.velX * elapsedSeconds);
        int y = static_cast<int>(piece.y + piece.velY * elapsedSeconds +
                                  0.5 * kGravityPxPerSec2 * elapsedSeconds * elapsedSeconds);
        int halfWidth = static_cast<int>(kMaxHalfWidthPx * std::abs(std::cos(piece.spinPhase + elapsedSeconds * 6.0)));
        DrawAlphaRect(hdc, x, y, std::max(halfWidth, 1), kHalfHeightPx, piece.color, fadeAlpha);
    }
}

// Draws the in-progress notes-exploding burst: each spark flies straight
// outward from wherever its note was, under exponential drag rather than
// gravity - decelerating to a stop rather than falling - fading out
// linearly over its short, fixed lifetime.
void NoteLaneGdiRenderer::DrawExplosion(HDC hdc, double elapsedSeconds, double t)
{
    constexpr int kParticleRadiusPx = 3;

    BYTE fadeAlpha = static_cast<BYTE>(ClampChannel(static_cast<int>(255 * (1.0 - t))));

    // Closed-form position under v(t) = v0*exp(-k*t): integrates to
    // x0 + (v0/k)*(1 - exp(-k*t)), asymptotically settling instead of
    // ever reversing or overshooting like a naive per-frame velocity
    // decay would.
    double factor = (1.0 - std::exp(-kExplosionDragPerSec * elapsedSeconds)) / kExplosionDragPerSec;

    for (const ExplosionParticle& particle : m_explosion)
    {
        int x = static_cast<int>(particle.x + particle.velX * factor);
        int y = static_cast<int>(particle.y + particle.velY * factor);
        DrawAlphaCircle(hdc, x, y, kParticleRadiusPx, particle.color, fadeAlpha);
    }
}

// Expanding judgement ripples (green for a hit, red for a pre-lock-in
// miss): grow outward at each ripple's own fixed speed from that lane's
// judge-line position, fading out continuously as they grow so the fade
// actually reads before the ring is gone, rather than staying near-opaque
// until it's clipped off the edge of the lane. The fade (and removal) is
// measured against the farthest single edge of the rect the ring has to
// cross (not the farthest corner, which is much further away along the
// diagonal and would leave the ring looking fully opaque for most of its
// life, fading only once it's already well past the visible lane).
void NoteLaneGdiRenderer::DrawRipples(HDC hdc, RECT laneRect)
{
    int lineY = LineY(laneRect);
    for (size_t i = 0; i < m_ripples.size();)
    {
        const JudgementRipple& ripple = m_ripples[i];
        int cx = LaneCenterX(laneRect, ripple.lane);
        int cy = lineY;
        double elapsedSeconds = (GetTickCount() - ripple.startMs) / 1000.0;
        double radius = elapsedSeconds * ripple.speedPxPerSec;

        double maxRadius = std::max({std::abs(cx - laneRect.left), std::abs(cx - laneRect.right),
                                      std::abs(cy - laneRect.top), std::abs(cy - laneRect.bottom)});

        if (radius > maxRadius)
        {
            m_ripples.erase(m_ripples.begin() + i);
            continue;
        }

        double fadeT = std::min(1.0, (radius / maxRadius) * ripple.fadeRate);
        BYTE alpha = static_cast<BYTE>(ClampChannel(static_cast<int>(ripple.startAlpha * (1.0 - fadeT))));
        DrawAlphaRing(hdc, cx, cy, static_cast<int>(radius), kRippleThicknessPx, ripple.color, alpha);
        ++i;
    }
}

// HUD: a translucent rounded panel with bold, drop-shadowed status text so
// it stays readable over the busy, colorful background beneath it.
// scoreText (right-aligned) gets first claim on the panel's width - status
// text (left-aligned) has its own rect shrunk to end before it starts (and
// ellipsizes via DT_END_ELLIPSIS if it still doesn't fit), so the two can
// never overlap regardless of how long either one is.
void NoteLaneGdiRenderer::DrawHud(HDC hdc, RECT laneRect, const std::wstring& statusText, const std::wstring& scoreText)
{
    RECT panelRect{laneRect.left + 8, laneRect.top + 8, laneRect.right - 8, laneRect.top + 44};
    DrawAlphaRoundRect(hdc, panelRect, 14, RGB(12, 8, 28), 175);

    if (!m_hudFont)
    {
        m_hudFont = CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, m_hudFont);
    SetBkMode(hdc, TRANSPARENT);

    RECT textRect = panelRect;
    textRect.left += 10;
    textRect.right -= 10;

    RECT scoreRect = textRect;
    if (!scoreText.empty())
    {
        scoreRect.left = std::max(scoreRect.left, scoreRect.right - 150);
        textRect.right = scoreRect.left - 8;
    }

    constexpr UINT kTextFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    constexpr UINT kScoreFlags = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;

    // Cheap drop shadow: draw the text once in near-black offset a pixel, then again on top.
    RECT shadowRect = textRect;
    shadowRect.left += 1;
    shadowRect.top += 1;
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextW(hdc, statusText.c_str(), -1, &shadowRect, kTextFlags);
    SetTextColor(hdc, kTextColor);
    DrawTextW(hdc, statusText.c_str(), -1, &textRect, kTextFlags);

    if (!scoreText.empty())
    {
        RECT scoreShadowRect = scoreRect;
        scoreShadowRect.left += 1;
        scoreShadowRect.top += 1;
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, scoreText.c_str(), -1, &scoreShadowRect, kScoreFlags);
        SetTextColor(hdc, kTextColor);
        DrawTextW(hdc, scoreText.c_str(), -1, &scoreRect, kScoreFlags);
    }

    SelectObject(hdc, oldFont);
}

// The hits meter panel: a translucent rounded track, same visual language
// as DrawHud's panel, with a colored fill anchored to the bottom that grows
// upward as scene.hitsMeterProgress climbs toward 1 - so it reads as
// filling "up towards passing," matching notes falling down into the judge
// line right beside it. Drawn only while scene.showHitsMeter is true; the
// instant that flips false (a lock-in), the panel simply stops being drawn
// here at all - Draw() spawns AppendHitsMeterExplosion's burst on that same
// frame instead, so it disappears with a little celebration rather than
// just popping away with nothing.
void NoteLaneGdiRenderer::DrawHitsMeter(HDC hdc, RECT hitsMeterRect, const NoteLaneScene& scene)
{
    if (!scene.showHitsMeter || hitsMeterRect.right <= hitsMeterRect.left ||
        hitsMeterRect.bottom <= hitsMeterRect.top)
    {
        return;
    }

    constexpr int kCornerRadius = 8;
    constexpr int kInsetPx = 3;

    DrawAlphaRoundRect(hdc, hitsMeterRect, kCornerRadius, RGB(12, 8, 28), 175);

    HPEN oldPen = (HPEN)SelectObject(hdc, CachedSolidPen(2, kBorderColor));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, hitsMeterRect.left, hitsMeterRect.top, hitsMeterRect.right, hitsMeterRect.bottom, kCornerRadius,
              kCornerRadius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);

    RECT innerRect{hitsMeterRect.left + kInsetPx, hitsMeterRect.top + kInsetPx, hitsMeterRect.right - kInsetPx,
                    hitsMeterRect.bottom - kInsetPx};
    int innerHeight = innerRect.bottom - innerRect.top;
    int fillHeight = static_cast<int>(innerHeight * scene.hitsMeterProgress);
    if (innerRect.right <= innerRect.left || fillHeight <= 0)
    {
        return;
    }

    COLORREF fillColor = scene.primaryClip ? scene.primaryClip->color : kStreakColor;
    RECT fillRect{innerRect.left, innerRect.bottom - fillHeight, innerRect.right, innerRect.bottom};
    DrawAlphaRoundRect(hdc, fillRect, kCornerRadius - 2, fillColor, 235);
}

void NoteLaneGdiRenderer::ToggleDebugOverlay()
{
    m_debugOverlayEnabled = !m_debugOverlayEnabled;
}

// Small panel below the status HUD showing NoteLaneModel's own
// previous/current/next instance chain by clip name - only ever drawn
// while m_debugOverlayEnabled (see ToggleDebugOverlay), purely a
// debugging aid for that chain's own behavior (loop-repeat prediction,
// promotion, retirement into previous).
void NoteLaneGdiRenderer::DrawDebugOverlay(HDC hdc, RECT laneRect, const NoteLaneScene& scene)
{
    RECT panelRect{laneRect.left + 8, laneRect.top + 50, laneRect.right - 8, laneRect.top + 104};
    DrawAlphaRoundRect(hdc, panelRect, 10, RGB(12, 8, 28), 175);

    if (!m_hudFont)
    {
        m_hudFont = CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, m_hudFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kTextColor);

    constexpr UINT kTextFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    std::wstring lines[3] = {L"prev: " + scene.debugPreviousClipName, L"cur:  " + scene.debugCurrentClipName,
                              L"next: " + scene.debugNextClipName};
    int lineHeight = (panelRect.bottom - panelRect.top) / 3;
    for (int i = 0; i < 3; ++i)
    {
        RECT lineRect{panelRect.left + 10, panelRect.top + i * lineHeight, panelRect.right - 10,
                       panelRect.top + (i + 1) * lineHeight};
        DrawTextW(hdc, lines[i].c_str(), -1, &lineRect, kTextFlags);
    }

    SelectObject(hdc, oldFont);
}

// The instant a track locks in, burst a confetti celebration across the
// full width of the lane. Positions/velocities/colors are all generated
// here from PseudoRandom() (same stable-scatter trick DrawSparkles uses)
// and simulated purely from elapsed time in DrawConfetti.
void NoteLaneGdiRenderer::SpawnConfetti(RECT laneRect)
{
    m_confetti.clear();
    m_confetti.reserve(kConfettiPieceCount);
    double width = laneRect.right - laneRect.left;
    for (int i = 0; i < kConfettiPieceCount; ++i)
    {
        double spawnX = laneRect.left + PseudoRandom(i * 7 + 1) * width;
        double spawnY = laneRect.top - PseudoRandom(i * 7 + 2) * 60.0;
        double velX = (PseudoRandom(i * 7 + 3) - 0.5) * 120.0;
        double velY = 60.0 + PseudoRandom(i * 7 + 4) * 80.0;
        double spinPhase = PseudoRandom(i * 7 + 5) * 6.283185307;
        COLORREF color = kConfettiPalette[i % kConfettiPaletteSize];
        m_confetti.push_back({spawnX, spawnY, velX, velY, spinPhase, color});
    }
    m_confettiStartMs = GetTickCount();
}

// Adds a smaller confetti burst scattered across hitsMeterRect's own width
// into m_confetti, on top of whatever SpawnConfetti just put there - meant
// to be called right after it, on the same justLockedIn frame (Pass mode
// only), so both bursts animate and clear together on SpawnConfetti's own
// timer (m_confettiStartMs) with no extra state of their own. i's own seed
// range starts well clear of SpawnConfetti's (i*7+1..5 up to
// kConfettiPieceCount) so the two bursts don't scatter identically.
void NoteLaneGdiRenderer::AppendHitsMeterConfetti(RECT hitsMeterRect)
{
    if (hitsMeterRect.right <= hitsMeterRect.left)
    {
        return;
    }
    double width = hitsMeterRect.right - hitsMeterRect.left;
    int seedBase = kConfettiPieceCount * 7 + 100;
    for (int i = 0; i < kHitsMeterConfettiPieceCount; ++i)
    {
        double spawnX = hitsMeterRect.left + PseudoRandom(seedBase + i * 7 + 1) * width;
        double spawnY = hitsMeterRect.top - PseudoRandom(seedBase + i * 7 + 2) * 40.0;
        double velX = (PseudoRandom(seedBase + i * 7 + 3) - 0.5) * 100.0;
        double velY = 50.0 + PseudoRandom(seedBase + i * 7 + 4) * 70.0;
        double spinPhase = PseudoRandom(seedBase + i * 7 + 5) * 6.283185307;
        COLORREF color = kConfettiPalette[i % kConfettiPaletteSize];
        m_confetti.push_back({spawnX, spawnY, velX, velY, spinPhase, color});
    }
}

// Bursts each of scene.explodingNotes that's actually on screen right now
// apart into a handful of sparks flying outward from its own position,
// instead of a generic confetti celebration or just vanishing with no
// visual treatment at all.
void NoteLaneGdiRenderer::SpawnExplosion(RECT laneRect, const NoteLaneScene& scene)
{
    m_explosion.clear();
    int particleSeed = 0;
    for (const SceneNote& note : scene.explodingNotes)
    {
        int laneX = LaneCenterX(laneRect, note.lane);
        int noteY = YForBeatsFromNow(laneRect, note.startBeat - scene.nowBeat);
        if (noteY < laneRect.top - kVisibilityMarginPx || noteY > laneRect.bottom + kVisibilityMarginPx)
        {
            continue;
        }
        COLORREF explosionColor = note.clip->color;
        for (int p = 0; p < kExplosionParticlesPerNote; ++p)
        {
            double angle = PseudoRandom(particleSeed++) * 6.283185307;
            double speed = kExplosionMinSpeedPxPerSec +
                           PseudoRandom(particleSeed++) * (kExplosionMaxSpeedPxPerSec - kExplosionMinSpeedPxPerSec);
            m_explosion.push_back({static_cast<double>(laneX), static_cast<double>(noteY), std::cos(angle) * speed,
                                    std::sin(angle) * speed, explosionColor});
        }
    }
    m_explosionStartMs = GetTickCount();
}

// Adds a burst of sparks flying outward from hitsMeterRect's own center
// into m_explosion, on top of whatever SpawnExplosion just put there -
// meant to be called right after it, on the same justLockedIn frame, so
// both bursts animate and clear together on SpawnExplosion's own timer
// (m_explosionStartMs) with no extra state of their own. particleSeed
// starts well clear of SpawnExplosion's own seed range (bounded by
// kExplosionParticlesPerNote times however many exploding notes there are)
// so the two bursts don't happen to scatter in an identical pattern.
void NoteLaneGdiRenderer::AppendHitsMeterExplosion(RECT hitsMeterRect, COLORREF color)
{
    if (hitsMeterRect.right <= hitsMeterRect.left || hitsMeterRect.bottom <= hitsMeterRect.top)
    {
        return;
    }
    int cx = (hitsMeterRect.left + hitsMeterRect.right) / 2;
    int cy = (hitsMeterRect.top + hitsMeterRect.bottom) / 2;
    int particleSeed = 9000;
    for (int p = 0; p < kHitsMeterExplosionParticleCount; ++p)
    {
        double angle = PseudoRandom(particleSeed++) * 6.283185307;
        double speed = kExplosionMinSpeedPxPerSec +
                       PseudoRandom(particleSeed++) * (kExplosionMaxSpeedPxPerSec - kExplosionMinSpeedPxPerSec);
        m_explosion.push_back(
            {static_cast<double>(cx), static_cast<double>(cy), std::cos(angle) * speed, std::sin(angle) * speed, color});
    }
}

// Flashes a brief hit/miss indicator at the judge line for one lane's
// column, and spawns an expanding judgement ripple: green for a precise hit,
// yellow for an imprecise-but-still-correct one (always, even while
// passing), red for a miss while not yet/no longer passing only.
void NoteLaneGdiRenderer::OnJudgement(JudgementResult result, int lane, bool passing, bool precise)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_flashResult[lane] = result;
    m_flashUntilMs[lane] = GetTickCount() + kFlashDurationMs;

    if (result == JudgementResult::Hit)
    {
        COLORREF rippleColor = precise ? kNoteColorHit : kNoteColorHitImprecise;
        m_ripples.push_back(
            {lane, GetTickCount(), rippleColor, kHitRippleSpeedPxPerSec, kHitRippleStartAlpha, kHitRippleFadeRate});
    }
    else if (result == JudgementResult::Miss && !passing)
    {
        m_ripples.push_back({lane, GetTickCount(), kNoteColorMiss, kMissRippleSpeedPxPerSec, kMissRippleStartAlpha,
                              kMissRippleFadeRate});
    }
}

// Paints the background, columns, receptors, notes, and status text for one
// scene, plus the hits meter panel beside it.
void NoteLaneGdiRenderer::Draw(HDC hdc, RECT laneRect, RECT hitsMeterRect, const NoteLaneScene& scene)
{
    // A percussive pulse value that's brightest right on the beat and
    // decays before the next one.
    double beatPhase = scene.clockRunning ? (scene.nowBeat - std::floor(scene.nowBeat)) : 0.0;
    double beatPulse = scene.clockRunning ? std::exp(-beatPhase * 6.0) : 0.0;

    // Background: a vertical gradient that breathes brighter right on the
    // beat, so part of the scene is always moving with the music even
    // where nothing is actively being judged.
    COLORREF bgTop = Lighten(kBgTop, static_cast<int>(beatPulse * 22));
    COLORREF bgBottom = Lighten(kBgBottom, static_cast<int>(beatPulse * 14));
    FillVerticalGradient(hdc, laneRect, bgTop, bgBottom);

    DrawSparkles(hdc, laneRect, beatPulse);

    HPEN oldBorderPen = (HPEN)SelectObject(hdc, CachedSolidPen(2, kBorderColor));
    HBRUSH oldBorderBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, laneRect.left, laneRect.top, laneRect.right, laneRect.bottom, 18, 18);
    SelectObject(hdc, oldBorderBrush);
    SelectObject(hdc, oldBorderPen);

    DrawMeasureLines(hdc, laneRect, scene);

    COLORREF primaryColor = scene.primaryClip ? scene.primaryClip->color : ClipColor::kNeutral;
    DrawRails(hdc, laneRect, primaryColor);

    // A Pass-mode section's own already-exploded notes (see
    // NoteLaneScene::passLineHitLanes) still get a real "hit" flash at the
    // judge line as each one crosses it - routed through the exact same
    // OnJudgement path a real judged Hit would use, so it reuses that
    // flash/ripple treatment verbatim instead of needing a visual
    // treatment of its own. Must run before DrawReceptors below, which is
    // what actually reads the flash state this sets.
    for (int lane : scene.passLineHitLanes)
    {
        OnJudgement(JudgementResult::Hit, lane, true, /*precise=*/true);
    }

    DrawReceptors(hdc, laneRect, scene, primaryColor);

    if (scene.justLockedIn)
    {
        SpawnConfetti(laneRect);
    }
    if (scene.justHandedOff || scene.justLockedIn || scene.justFailed)
    {
        SpawnExplosion(laneRect, scene);
    }
    if (scene.justLockedIn)
    {
        // The hits meter is about to stop being drawn this same frame (see
        // DrawHitsMeter) - give it a burst of its own instead of just
        // vanishing. DontFail gets the small spark burst every time (it can
        // reappear and disappear many times over one section's run); Pass
        // only ever disappears once for the whole section (passing is a
        // one-way latch), so it gets its own confetti burst instead - on
        // top of, not instead of, SpawnConfetti's playfield-wide one above.
        if (scene.hitsMeterIsDontFail)
        {
            AppendHitsMeterExplosion(hitsMeterRect, kNoteColorHit);
        }
        else
        {
            AppendHitsMeterConfetti(hitsMeterRect);
        }
    }

    // A newly-spawned note's top edge extends above the lane by its own
    // duration (only its bottom edge is checked for visibility in
    // DrawNotes), so without clipping it draws onto whatever's above the
    // lane. Scoped to just DrawNotes so it doesn't also clip the confetti
    // burst, which deliberately spawns above the lane and falls in.
    int savedClipState = SaveDC(hdc);
    IntersectClipRect(hdc, laneRect.left, laneRect.top, laneRect.right, laneRect.bottom);
    DrawNotes(hdc, laneRect, scene);
    RestoreDC(hdc, savedClipState);

    if (!m_confetti.empty())
    {
        DWORD elapsed = GetTickCount() - m_confettiStartMs;
        if (elapsed >= kConfettiDurationMs)
        {
            m_confetti.clear();
        }
        else
        {
            double t = elapsed / static_cast<double>(kConfettiDurationMs);
            DrawConfetti(hdc, elapsed / 1000.0, t);
        }
    }

    if (!m_explosion.empty())
    {
        DWORD elapsed = GetTickCount() - m_explosionStartMs;
        if (elapsed >= kExplosionDurationMs)
        {
            m_explosion.clear();
        }
        else
        {
            double t = elapsed / static_cast<double>(kExplosionDurationMs);
            DrawExplosion(hdc, elapsed / 1000.0, t);
        }
    }

    DrawRipples(hdc, laneRect);
    DrawHud(hdc, laneRect, scene.statusText, scene.scoreText);
    DrawHitsMeter(hdc, hitsMeterRect, scene);
    if (m_debugOverlayEnabled)
    {
        DrawDebugOverlay(hdc, laneRect, scene);
    }
}
