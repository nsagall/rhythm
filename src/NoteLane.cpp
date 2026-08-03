#include "NoteLane.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "ColorUtil.h"

using ColorUtil::ClampChannel;
using ColorUtil::Darken;
using ColorUtil::Lighten;

namespace
{

// GLOBAL SWITCH: when true, a section's dots hand off early to the next
// clip's preview - as soon as its own notes are due to become visible -
// which is what makes the current section's notes disappear before that
// next section actually starts. Set to false (the current setting) to
// disable that early handoff entirely: a section's own dots and judging
// now stay live for its whole duration, right up until the literal
// scheduled advance, instead of giving way in anticipation of it. See
// nextClipShowing's own computation in Draw() for where this is read.
constexpr bool kPreviewNextClipBeforeHandoff = false;

constexpr UINT_PTR kFrameTimerId = 400;
constexpr UINT kFrameIntervalMs = 16;
constexpr int kVisibilityMarginPx = 12;
constexpr DWORD kFlashDurationMs = 220;
constexpr DWORD kConfettiDurationMs = 1400;
constexpr int kConfettiPieceCount = 60;

// Notes-exploding burst (see ExplosionParticle) - shorter and snappier
// than the confetti burst since it's marking a bunch of individual notes
// popping at once, not one big celebration.
constexpr DWORD kExplosionDurationMs = 500;
constexpr int kExplosionParticlesPerNote = 10;
constexpr double kExplosionMinSpeedPxPerSec = 90.0;
constexpr double kExplosionMaxSpeedPxPerSec = 260.0;
constexpr double kExplosionDragPerSec = 2.5; // exponential slowdown - a burst outward that settles, not a straight-line fly-off

// A miss ripple's speed was originally 260 px/sec; it now takes 75% as long
// to grow past the lane and disappear, so it travels the same distance in
// 75% of the time - i.e. 1/0.75 as fast. A hit ripple takes half as long
// again as that (50% of the miss ripple's now-current disappear time), so
// it's twice as fast still.
constexpr double kMissRippleSpeedPxPerSec = 260.0 / 0.75;
constexpr double kHitRippleSpeedPxPerSec = kMissRippleSpeedPxPerSec / 0.5;
constexpr int kRippleThicknessPx = 4;

// Starting opacity for each ripple color - both fade further from here as
// they grow (see the fade math where m_ripples is drawn), never brighter.
constexpr int kHitRippleStartAlpha = static_cast<int>(255 * 0.5);   // 50% transparent to start
constexpr int kMissRippleStartAlpha = static_cast<int>(255 * 0.25); // 75% transparent to start

// A hit ripple's alpha reaches zero at half the radius (so half the time)
// a miss ripple's does - it fades away twice as fast, independent of how
// fast either one is still visibly growing/traveling toward off-screen.
constexpr double kHitRippleFadeRate = 2.0;
constexpr double kMissRippleFadeRate = 1.0;

// How much of the pattern timeline is visible across the lane: notes up to
// kBeatsAhead (== kNoteFallBeats, LaneConfig.h - shared with GameSession's
// lock-in advance-timing floor) beats in the future spawn at the top edge,
// and notes up to kBeatsBehind beats in the past remain visible before
// scrolling off the bottom edge. The judge line sits at beatsFromNow == 0.
constexpr double kBeatsAhead = kNoteFallBeats;
constexpr double kBeatsBehind = 1.0;

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

// One accent color per lane - a small curated "candy" palette (magenta,
// electric cyan, gold, violet) rather than a literal rainbow, so the lane
// colors read as a matched set instead of clashing. Deliberately avoids red
// and green altogether so no lane's own color can be mistaken for a
// kNoteColorHit/kNoteColorMiss pass/fail flash - lane 0 in particular is
// pushed toward magenta (hue ~307 deg) rather than a pinker, redder hue, so
// it doesn't read as a near-miss of kNoteColorMiss's red at a glance.
constexpr COLORREF kLaneColors[kLaneCount] = {
    RGB(255, 70, 235),
    RGB(56, 219, 255),
    RGB(255, 205, 70),
    RGB(190, 140, 255),
};

// Pass/fail feedback: saturated, high-contrast green/red, pushed toward
// each color's most vivid extreme (rather than a softer pastel) so the
// judgement reads instantly at a glance - paired with a glow halo (see the
// isJudgedNote glow in the dots-drawing loop below) so it pops rather than
// just being a flat color swap. kNoteColorHit doubles as the "correctly
// held, not yet released" color - the note turns green the instant a
// press starts it within tolerance, not only once the hold resolves.
constexpr COLORREF kNoteColorHit = RGB(40, 235, 80);
constexpr COLORREF kNoteColorMiss = RGB(255, 30, 30);

// Palette confetti pieces are drawn from at lock-in - the lane palette plus
// a couple of neutral accents, so the burst reads as "party colors" without
// introducing any new color meaning of its own.
constexpr COLORREF kConfettiPalette[] = {
    kLaneColors[0], kLaneColors[1], kLaneColors[2], kLaneColors[3], kStreakColor, RGB(255, 255, 255),
};
constexpr int kConfettiPaletteSize = sizeof(kConfettiPalette) / sizeof(kConfettiPalette[0]);

// One note visible on screen for a single lane: where it starts and how long it lasts, both in beats.
struct VisibleNote
{
    double startBeat = 0.0;
    double durationBeats = 0.0;
};

// Returns every note (from one lane's repeating pattern) whose bar - from
// its start to its start+duration - overlaps [fromBeat, toBeat] at all, not
// just ones whose start falls inside it: a note that started before
// fromBeat but whose duration carries its tail past fromBeat is still
// (partly) on screen and must stay visible until its tail actually
// scrolls off, not disappear the instant its start alone falls behind
// the visible window.
std::vector<VisibleNote> NotesInRange(double fromBeat, double toBeat, const std::vector<LaneNote>& notes,
                                       double spanBeats)
{
    std::vector<VisibleNote> result;
    if (notes.empty() || spanBeats <= 0.0)
    {
        return result;
    }

    long long firstBar = static_cast<long long>(std::floor(fromBeat / spanBeats)) - 1;
    long long lastBar = static_cast<long long>(std::floor(toBeat / spanBeats)) + 1;

    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (const LaneNote& note : notes)
        {
            double absoluteStart = bar * spanBeats + note.startBeat;
            double absoluteEnd = absoluteStart + note.durationBeats;
            if (absoluteEnd >= fromBeat && absoluteStart <= toBeat)
            {
                result.push_back({absoluteStart, note.durationBeats});
            }
        }
    }
    return result;
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

// Returns a solid brush for `color`, creating it once and reusing it for
// the rest of the process's life - every note/receptor/ripple/confetti
// piece redraws every single frame (up to 60 times a second) from a
// small, bounded palette (lane colors, hit/miss/locked-miss, and their
// Darken()/Lighten() derivatives), so creating and immediately destroying
// a fresh HBRUSH for each one was pure per-frame GDI churn for no benefit.
// Never freed - like the file's cached HFONTs, these live for the process.
HBRUSH CachedSolidBrush(COLORREF color)
{
    static std::unordered_map<COLORREF, HBRUSH> cache;
    HBRUSH& brush = cache[color];
    if (!brush)
    {
        brush = CreateSolidBrush(color);
    }
    return brush;
}

// Same idea as CachedSolidBrush, for the PS_SOLID pens used to outline
// rings and the lane border (thickness, color) pairs are the same small,
// bounded set every frame.
HPEN CachedSolidPen(int width, COLORREF color)
{
    static std::unordered_map<UINT64, HPEN> cache;
    UINT64 key = (static_cast<UINT64>(static_cast<UINT32>(width)) << 32) | static_cast<UINT32>(color);
    HPEN& pen = cache[key];
    if (!pen)
    {
        pen = CreatePen(PS_SOLID, width, color);
    }
    return pen;
}

// A single scratch compatible-DC/bitmap shared by every AlphaShape use,
// grown (never shrunk) to the largest bounds any shape has needed so far.
// AlphaShape instances are always fully used up (constructed, drawn into,
// blended) one at a time before the next is created - Draw() never nests
// two of them - so one shared buffer is safe to reuse instead of every
// alpha-blended shape (there can be dozens per frame: every note glyph/
// bar, sparkle, receptor, confetti piece, ripple...) creating and tearing
// down its own compatible DC/bitmap from scratch.
struct ScratchBuffer
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP defaultBitmap = nullptr; // the DC's own placeholder bitmap, restored before growing/replacing
    int width = 0;
    int height = 0;
};

ScratchBuffer& EnsureScratchBuffer(HDC referenceHdc, int width, int height)
{
    static ScratchBuffer scratch;
    if (!scratch.dc)
    {
        scratch.dc = CreateCompatibleDC(referenceHdc);
    }
    if (width > scratch.width || height > scratch.height)
    {
        if (scratch.bitmap)
        {
            SelectObject(scratch.dc, scratch.defaultBitmap);
            DeleteObject(scratch.bitmap);
        }
        scratch.width = std::max(width, scratch.width);
        scratch.height = std::max(height, scratch.height);
        scratch.bitmap = CreateCompatibleBitmap(referenceHdc, scratch.width, scratch.height);
        HBITMAP previous = (HBITMAP)SelectObject(scratch.dc, scratch.bitmap);
        if (!scratch.defaultBitmap)
        {
            scratch.defaultBitmap = previous;
        }
    }
    return scratch;
}

// Alpha-blends a filled shape over whatever is already at (left,top,right,bottom), by capturing
// that region first so blending is correct against a gradient/busy background rather than an
// assumed flat color - GDI has no real per-primitive transparency, so this fakes it: copy the
// destination into the shared scratch bitmap, draw the shape solid on top of that copy, then
// AlphaBlend the scratch rect back - anywhere the shape didn't cover, scratch==destination,
// so blending is a no-op there regardless of alpha, and only the shape itself visibly blends in.
class AlphaShape
{
public:
    AlphaShape(HDC destHdc, RECT bounds) : m_destHdc(destHdc), m_bounds(bounds)
    {
        m_width = bounds.right - bounds.left;
        m_height = bounds.bottom - bounds.top;
        if (m_width <= 0 || m_height <= 0)
        {
            return;
        }
        m_memHdc = EnsureScratchBuffer(destHdc, m_width, m_height).dc;
        BitBlt(m_memHdc, 0, 0, m_width, m_height, destHdc, bounds.left, bounds.top, SRCCOPY);
    }

    HDC Dc() const
    {
        return m_memHdc;
    }

    // Blends the captured+drawn-on scratch bitmap back onto the destination at the given alpha (0-255).
    void Blend(BYTE alpha) const
    {
        if (!m_memHdc || alpha == 0)
        {
            return;
        }
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
        AlphaBlend(m_destHdc, m_bounds.left, m_bounds.top, m_width, m_height, m_memHdc, 0, 0, m_width, m_height,
                   blend);
    }

private:
    HDC m_destHdc = nullptr;
    RECT m_bounds{};
    int m_width = 0;
    int m_height = 0;
    HDC m_memHdc = nullptr;
};

// Alpha-blends a filled circle centered at (cx, cy) with the given radius/color/alpha.
void DrawAlphaCircle(HDC hdc, int cx, int cy, int radius, COLORREF color, BYTE alpha)
{
    if (radius <= 0)
    {
        return;
    }
    RECT bounds{cx - radius, cy - radius, cx + radius, cy + radius};
    AlphaShape shape(hdc, bounds);
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
void DrawAlphaRing(HDC hdc, int cx, int cy, int radius, int thickness, COLORREF color, BYTE alpha)
{
    if (radius <= 0)
    {
        return;
    }
    RECT bounds{cx - radius, cy - radius, cx + radius, cy + radius};
    AlphaShape shape(hdc, bounds);
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

// Alpha-blends a filled rounded rectangle - used for the translucent HUD panel.
void DrawAlphaRoundRect(HDC hdc, RECT rect, int cornerRadius, COLORREF color, BYTE alpha)
{
    AlphaShape shape(hdc, rect);
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
// half-width/half-height/color/alpha - used for confetti pieces.
void DrawAlphaRect(HDC hdc, int cx, int cy, int halfWidth, int halfHeight, COLORREF color, BYTE alpha)
{
    if (halfWidth <= 0 || halfHeight <= 0)
    {
        return;
    }
    RECT bounds{cx - halfWidth, cy - halfHeight, cx + halfWidth, cy + halfHeight};
    AlphaShape shape(hdc, bounds);
    if (!shape.Dc())
    {
        return;
    }
    RECT local{0, 0, halfWidth * 2, halfHeight * 2};
    FillRect(shape.Dc(), &local, CachedSolidBrush(color));
    shape.Blend(alpha);
}

// Cheap, stable pseudo-random float in [0,1) derived from an integer seed - used to scatter
// background sparkles at fixed-looking-random positions without needing <random> state.
double PseudoRandom(int seed)
{
    unsigned int x = static_cast<unsigned int>(seed) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return (x % 10000) / 10000.0;
}

// Draws a music-notation-free notehead marker: dark bevel rim, color fill, a soft alpha glow
// behind it, and a small specular highlight - the note's start marker, sitting at the bottom
// (leading) edge of its duration bar. lockedIn adds a second, wider green glow ring underneath
// everything else - a supplementary "this track has locked in" cue that doesn't touch color.
void DrawNoteGlyph(HDC hdc, int x, int y, COLORREF color, bool glow, bool lockedIn)
{
    if (lockedIn)
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
// lockedIn adds a glowing green outline around the whole bar underneath
// everything else - a supplementary "this track has locked in" cue that
// doesn't touch color, matching DrawNoteGlyph's own lockedIn glow.
void DrawNoteBar(HDC hdc, int x, int yTop, int yBottom, int halfWidth, COLORREF color, bool lockedIn)
{
    if (yBottom <= yTop)
    {
        return;
    }

    if (lockedIn)
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
void DrawReceptor(HDC hdc, int x, int y, COLORREF laneColor, bool held, bool flashing, COLORREF flashColor,
                   double flashProgress)
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

// Draws the in-progress lock-in confetti burst: each piece falls under
// simple gravity from its spawn position with a bit of horizontal drift,
// tumbling (a cheap width-scaling trick standing in for a spin, since GDI
// has no easy per-primitive rotation here) and fading out over its last
// stretch of life.
void DrawConfetti(HDC hdc, const std::vector<ConfettiPiece>& pieces, double elapsedSeconds, double t)
{
    constexpr double kGravityPxPerSec2 = 420.0;
    constexpr int kHalfHeightPx = 5;
    constexpr int kMaxHalfWidthPx = 4;

    BYTE fadeAlpha = static_cast<BYTE>(ClampChannel(static_cast<int>(255 * (1.0 - t * t))));

    for (const ConfettiPiece& piece : pieces)
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
void DrawExplosion(HDC hdc, const std::vector<ExplosionParticle>& particles, double elapsedSeconds, double t)
{
    constexpr int kParticleRadiusPx = 3;

    BYTE fadeAlpha = static_cast<BYTE>(ClampChannel(static_cast<int>(255 * (1.0 - t))));

    // Closed-form position under v(t) = v0*exp(-k*t): integrates to
    // x0 + (v0/k)*(1 - exp(-k*t)), asymptotically settling instead of
    // ever reversing or overshooting like a naive per-frame velocity
    // decay would.
    double factor = (1.0 - std::exp(-kExplosionDragPerSec * elapsedSeconds)) / kExplosionDragPerSec;

    for (const ExplosionParticle& particle : particles)
    {
        int x = static_cast<int>(particle.x + particle.velX * factor);
        int y = static_cast<int>(particle.y + particle.velY * factor);
        DrawAlphaCircle(hdc, x, y, kParticleRadiusPx, particle.color, fadeAlpha);
    }
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

// Flashes a brief hit/miss indicator at the judge line for one lane's
// column, and spawns an expanding judgement ripple: green for a hit
// (always, even after lock-in), red for a pre-lock-in miss only.
void NoteLane::ShowJudgement(JudgementResult result, int lane, bool lockedIn)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_flashResult[lane] = result;
    m_flashUntilMs[lane] = GetTickCount() + kFlashDurationMs;

    if (result == JudgementResult::Hit)
    {
        m_ripples.push_back(
            {lane, GetTickCount(), kNoteColorHit, kHitRippleSpeedPxPerSec, kHitRippleStartAlpha, kHitRippleFadeRate});
    }
    else if (result == JudgementResult::Miss && !lockedIn)
    {
        m_ripples.push_back({lane, GetTickCount(), kNoteColorMiss, kMissRippleSpeedPxPerSec, kMissRippleStartAlpha,
                              kMissRippleFadeRate});
    }
}

// Paints the background, columns, receptors, upcoming notes, and status text for the given session.
void NoteLane::Draw(HDC hdc, const GameSession& session)
{
    // How far into the current beat we are, and a percussive pulse value
    // that's brightest right on the beat and decays before the next one -
    // only meaningful once the song clock is actually running (SongClock
    // divides by an as-yet-unset frequency before Start(), so this must
    // stay gated on phase, not just called unconditionally).
    bool clockRunning = session.Phase() != GamePhase::Idle;
    double nowBeat = clockRunning ? session.Clock().BeatPosition() : 0.0;
    double beatPhase = clockRunning ? (nowBeat - std::floor(nowBeat)) : 0.0;
    double beatPulse = clockRunning ? std::exp(-beatPhase * 6.0) : 0.0;

    // Background: a vertical gradient that breathes brighter right on the
    // beat, so part of the scene is always moving with the music even
    // where nothing is actively being judged.
    COLORREF bgTop = Lighten(kBgTop, static_cast<int>(beatPulse * 22));
    COLORREF bgBottom = Lighten(kBgBottom, static_cast<int>(beatPulse * 14));
    FillVerticalGradient(hdc, m_laneRect, bgTop, bgBottom);

    double laneHeight = m_laneRect.bottom - m_laneRect.top;
    double pixelsPerBeat = laneHeight / (kBeatsAhead + kBeatsBehind);
    int lineY = m_laneRect.top + static_cast<int>(kBeatsAhead * pixelsPerBeat);

    // A scattering of small twinkling sparkles for ambiance - drift gently
    // on wall-clock time (alive even before the player presses Start) and
    // get an extra brightness kick from the beat pulse once the song runs.
    {
        DWORD ticks = GetTickCount();
        for (int i = 0; i < 14; ++i)
        {
            double fx = PseudoRandom(i * 3 + 1);
            double fy = PseudoRandom(i * 3 + 2);
            double phaseOffset = PseudoRandom(i * 3 + 3) * 6.283185307;
            double twinkle = 0.5 + 0.5 * std::sin(ticks / 900.0 + phaseOffset);
            int alpha = static_cast<int>(30 + twinkle * 70 + beatPulse * 60);
            int sx = m_laneRect.left + static_cast<int>(fx * (m_laneRect.right - m_laneRect.left));
            int sy = m_laneRect.top + static_cast<int>(fy * (m_laneRect.bottom - m_laneRect.top));
            int radius = (i % 3 == 0) ? 2 : 1;
            DrawAlphaCircle(hdc, sx, sy, radius, RGB(255, 255, 255), static_cast<BYTE>(ClampChannel(alpha)));
        }
    }

    HPEN oldBorderPen = (HPEN)SelectObject(hdc, CachedSolidPen(2, kBorderColor));
    HBRUSH oldBorderBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, m_laneRect.left, m_laneRect.top, m_laneRect.right, m_laneRect.bottom, 18, 18);
    SelectObject(hdc, oldBorderBrush);
    SelectObject(hdc, oldBorderPen);

    // Notes spawn at the top (the future) and scroll straight down toward
    // the judge line near the bottom: kBeatsAhead of the timeline occupies
    // the space above the line, kBeatsBehind the space below it. Shared
    // across every column - only the horizontal position varies by lane.
    double totalWidth = m_laneRect.right - m_laneRect.left;
    double columnWidth = (totalWidth - kColumnGutterPx * (kLaneCount - 1)) / kLaneCount;
    int barHalfWidth = static_cast<int>(columnWidth * kBarWidthFraction / 2.0);

    auto laneCenterX = [&](int lane)
    {
        return m_laneRect.left + static_cast<int>(lane * (columnWidth + kColumnGutterPx) + columnWidth / 2.0);
    };
    auto yForBeatsFromNow = [&](double beatsFromNow)
    {
        return m_laneRect.top + static_cast<int>((kBeatsAhead - beatsFromNow) * pixelsPerBeat);
    };

    // A faint horizontal line at every measure boundary crossing the
    // visible window, so the falling pattern reads against the song's own
    // bar grid instead of floating with no sense of scale. Drawn behind
    // the rails/notes below, inset slightly from the rounded border so it
    // doesn't clip oddly into the corners.
    int beatsPerBar = session.Song().beatsPerBar;
    if (beatsPerBar > 0)
    {
        double rangeStart = nowBeat - kBeatsBehind;
        double rangeEnd = nowBeat + kBeatsAhead;
        long long firstMeasure = static_cast<long long>(std::floor(rangeStart / beatsPerBar));
        long long lastMeasure = static_cast<long long>(std::ceil(rangeEnd / beatsPerBar));
        for (long long measure = firstMeasure; measure <= lastMeasure; ++measure)
        {
            double measureBeat = measure * beatsPerBar;
            if (measureBeat < rangeStart || measureBeat > rangeEnd)
            {
                continue;
            }
            int y = yForBeatsFromNow(measureBeat - nowBeat);
            int cx = (m_laneRect.left + m_laneRect.right) / 2;
            int halfWidth = (m_laneRect.right - m_laneRect.left) / 2 - 10;
            DrawAlphaRect(hdc, cx, y, halfWidth, 1, RGB(255, 255, 255), 28);
        }
    }

    // A faint glowing rail down each column, in that lane's color, so every
    // lane has a visible identity even where no notes are currently on screen.
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        int x = laneCenterX(lane);
        RECT railRect{x - 2, m_laneRect.top + 10, x + 2, lineY};
        DrawAlphaRoundRect(hdc, railRect, 2, kLaneColors[lane], 26);
    }

    const ChartClip* clip = session.CurrentClip();

    // A learn section's dots don't vanish the instant it ends - they keep
    // coming (and keep being judged) until the *next* clip's own dots
    // actually start scrolling in at the top of the lane - i.e. until the
    // earliest of its lanes' first notes (PreviewFirstOnsetBeatForLane)
    // crosses within kBeatsAhead of now, the same visibility threshold any
    // note uses (only while kPreviewNextClipBeforeHandoff is true - see its
    // own comment; currently false, so this section stays live right up to
    // the scheduled advance instead, same as the "nothing to preview"
    // branch below always did). That earliest onset is always at or after
    // the scheduled advance beat (it's found by searching forward from
    // there), so it can land anywhere from "already at the advance beat"
    // (common - explodes right as the handoff happens) to several beats
    // past it (a sparser clip) - waiting on it directly is what makes this
    // "only explode when something is actually arriving," not a fixed
    // time-based guess. The one thing that can't be allowed is waiting on
    // it *past* the advance beat itself - falls back to the advance beat
    // itself in that case (and whenever there's nothing to preview at all -
    // end of chart, the next section is a break/reset, or the next learn
    // section hides its own dots behind an intro), matching the old,
    // simpler "explode right at the handoff" behavior.
    bool isLearnSection =
        clip && session.Phase() == GamePhase::Learning && session.CurrentSectionKind() == SectionKind::Learn;
    bool nextClipShowing = false;

    // Caps how far into the future the live note-drawing loop below tiles
    // this clip's own repeating pattern - nowBeat + kBeatsAhead by default,
    // same as always. Only ever tightened, and only in the "nothing to
    // hand off to" branch just below: past the scheduled advance, this
    // clip's pattern would otherwise keep tiling forward as if it loops
    // again, since NotesInRange has no idea the section is actually about
    // to end there - drawing a phantom repeat of notes that will never be
    // reached (the section advances to a break/reset, or the chart ends,
    // before that repeat would ever play).
    double notesUpperBoundBeat = nowBeat + kBeatsAhead;

    if (isLearnSection)
    {
        double secondsPerBeat = 60.0 / session.Song().bpm;
        double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;

        if (!kPreviewNextClipBeforeHandoff || session.PreviewClip() == nullptr)
        {
            // Nothing to hand off to (end of chart, the next section is a
            // break/reset, or the next learn section hides its own dots
            // behind an intro) - keep this clip's own dots (and judging) live right
            // up until the actual scheduled advance, exactly like the
            // fallback below does when there IS a preview but it isn't
            // visible yet. Without this, locking in immediately made
            // nextClipShowing true - and with it, dotsClip null - for the
            // *entire* wait until the real advance, which could be many
            // seconds with loop_count > 1: every note still due kept
            // getting judged (Update() doesn't care what's drawn) but
            // simply stopped being drawn at all. Also always taken (skipping
            // the early-handoff branch below entirely) while
            // kPreviewNextClipBeforeHandoff is false, regardless of whether
            // there's a real preview to show.
            nextClipShowing = (nowBeat >= advanceAtBeat);
            notesUpperBoundBeat = std::min(notesUpperBoundBeat, advanceAtBeat);
        }
        else
        {
            double earliestOnset = -1.0;
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                double onset = session.PreviewFirstOnsetBeatForLane(lane);
                if (onset >= 0.0 && (earliestOnset < 0.0 || onset < earliestOnset))
                {
                    earliestOnset = onset;
                }
            }

            double visibleAtBeat = (earliestOnset >= 0.0) ? (earliestOnset - kBeatsAhead) : (advanceAtBeat - kBeatsAhead);
            double triggerBeat = (visibleAtBeat <= advanceAtBeat) ? visibleAtBeat : (advanceAtBeat - kBeatsAhead);
            nextClipShowing = (nowBeat >= triggerBeat);
        }
    }

    bool isLiveJudging = isLearnSection && !nextClipShowing;

    // Judge-line receptors: one glowing ring per lane, dim by default,
    // glowing while a note is held, and popping brightly for a moment when
    // that lane's press/release was just judged.
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        int x = laneCenterX(lane);
        bool held = isLiveJudging && session.IsLaneHeld(lane);
        bool flashing = GetTickCount() < m_flashUntilMs[lane];
        double flashProgress = 0.0;
        COLORREF flashColor = kNoteColorHit;
        if (flashing)
        {
            DWORD remaining = m_flashUntilMs[lane] - GetTickCount();
            flashProgress = 1.0 - (static_cast<double>(remaining) / kFlashDurationMs);
            flashColor = (m_flashResult[lane] == JudgementResult::Hit) ? kNoteColorHit : kNoteColorMiss;
        }
        DrawReceptor(hdc, x, lineY, kLaneColors[lane], held, flashing, flashColor, flashProgress);
    }

    // The instant a track locks in (its streak reaches the clip's own
    // hits_required - session.IsLockedIn() flips false->true), burst a
    // confetti celebration across the full width of the lane.
    // Positions/velocities/colors are all generated here from
    // PseudoRandom() (same stable-scatter trick the background sparkles
    // use) and simulated purely from elapsed time in DrawConfetti.
    bool nowLockedIn = isLearnSection && session.IsLockedIn();
    bool justLockedIn = nowLockedIn && !m_prevLockedIn;
    if (justLockedIn)
    {
        m_confetti.clear();
        m_confetti.reserve(kConfettiPieceCount);
        double width = m_laneRect.right - m_laneRect.left;
        for (int i = 0; i < kConfettiPieceCount; ++i)
        {
            double spawnX = m_laneRect.left + PseudoRandom(i * 7 + 1) * width;
            double spawnY = m_laneRect.top - PseudoRandom(i * 7 + 2) * 60.0;
            double velX = (PseudoRandom(i * 7 + 3) - 0.5) * 120.0;
            double velY = 60.0 + PseudoRandom(i * 7 + 4) * 80.0;
            double spinPhase = PseudoRandom(i * 7 + 5) * 6.283185307;
            COLORREF color = kConfettiPalette[i % kConfettiPaletteSize];
            m_confetti.push_back({spawnX, spawnY, velX, velY, spinPhase, color});
        }
        m_confettiStartMs = GetTickCount();
    }

    // The instant a locked-in track's dots actually give way to the next
    // clip's preview (nextClipShowing flips false->true), whatever notes
    // of the outgoing clip are still on screen right then burst apart at
    // their own positions - a handful of sparks flying outward from each
    // one - instead of a generic confetti celebration or just vanishing
    // with no visual treatment at all. Mirrors the same
    // dotsFromBeat/NotesInRange/yForBeatsFromNow the live note-drawing
    // loop below uses, since these are exactly the notes that loop would
    // have drawn one frame earlier (isLiveJudging just flipped false).
    bool justHandedOff = nextClipShowing && !m_prevNotesHandoff;
    if (justHandedOff && clip)
    {
        m_explosion.clear();
        int particleSeed = 0;
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            int laneX = laneCenterX(lane);
            for (const VisibleNote& note :
                 NotesInRange(nowBeat - kBeatsBehind, notesUpperBoundBeat, clip->laneNotes[lane], clip->spanBeats))
            {
                int noteY = yForBeatsFromNow(note.startBeat - nowBeat);
                if (noteY < m_laneRect.top - kVisibilityMarginPx || noteY > m_laneRect.bottom + kVisibilityMarginPx)
                {
                    continue;
                }
                for (int p = 0; p < kExplosionParticlesPerNote; ++p)
                {
                    double angle = PseudoRandom(particleSeed++) * 6.283185307;
                    double speed = kExplosionMinSpeedPxPerSec +
                                   PseudoRandom(particleSeed++) * (kExplosionMaxSpeedPxPerSec - kExplosionMinSpeedPxPerSec);
                    m_explosion.push_back({static_cast<double>(laneX), static_cast<double>(noteY),
                                            std::cos(angle) * speed, std::sin(angle) * speed, kLaneColors[lane]});
                }
            }
        }
        m_explosionStartMs = GetTickCount();
    }
    m_prevLockedIn = nowLockedIn;
    m_prevNotesHandoff = nextClipShowing;

    // While the player can't act yet (count-in, a clip's intro, a
    // break/reset/background section, or the wait before the next clip joins),
    // start showing the upcoming clip's notes from each lane's own actual
    // first required note onward - so each one scrolls in from the top
    // edge one at a time, exactly like live play, instead of a whole batch
    // of already-partially-scrolled-in notes popping in together once
    // revealed.
    const ChartClip* dotsClip = nullptr;
    if (isLiveJudging)
    {
        dotsClip = clip;
    }
    else
    {
        dotsClip = session.PreviewClip();
    }

    // Once a track has locked in (still live-judging through its extended
    // post-lock-in run - see isLiveJudging/nextClipShowing above), every
    // one of its notes gets a glowing green outline added on top of its
    // usual color - held/hit/miss/upcoming all keep reading exactly as
    // they normally would, the glow is purely an additional "this track
    // has already locked in" cue, not a replacement for the note's own
    // color.
    bool lockedIn = isLiveJudging && session.IsLockedIn();

    // A note's bar is drawn from its start (bottom edge) to its start+
    // duration (top edge) - the moment it first becomes visible, its start
    // sits exactly at the top of the lane, so its top edge necessarily
    // extends *above* that by its own duration. The visibility check just
    // below only tests a note's bottom edge against the lane's top (there's
    // deliberately no equivalent check on the top edge, since a note is
    // still meant to be at least partly visible then), so without clipping
    // that excess draws straight onto whatever's above the lane - visible
    // as a static-looking block for any lane with a note on every beat,
    // since a new one is always mid-spawn. Scoped to just the note-drawing
    // loop (not the whole function) so it doesn't also clip the confetti
    // burst below, which deliberately spawns above the lane and falls in.
    int savedClipState = SaveDC(hdc);
    IntersectClipRect(hdc, m_laneRect.left, m_laneRect.top, m_laneRect.right, m_laneRect.bottom);

    if (dotsClip)
    {
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            double dotsFromBeat;
            if (isLiveJudging)
            {
                dotsFromBeat = nowBeat - kBeatsBehind;
            }
            else
            {
                dotsFromBeat = session.PreviewFirstOnsetBeatForLane(lane);
                if (dotsFromBeat < 0.0)
                {
                    continue;
                }
            }

            int laneX = laneCenterX(lane);
            for (const VisibleNote& note :
                 NotesInRange(dotsFromBeat, notesUpperBoundBeat, dotsClip->laneNotes[lane], dotsClip->spanBeats))
            {
                double beatsFromStart = note.startBeat - nowBeat;
                double beatsFromEnd = beatsFromStart + note.durationBeats;
                int yBottom = yForBeatsFromNow(beatsFromStart); // the start - the leading, lower edge
                int yTop = yForBeatsFromNow(beatsFromEnd);      // the end - the trailing, upper edge
                if (yBottom < m_laneRect.top - kVisibilityMarginPx || yTop > m_laneRect.bottom + kVisibilityMarginPx)
                {
                    continue;
                }

                // Upcoming notes stay that lane's own color. The instant a
                // press starts it correctly (within tolerance), it turns
                // green and stays green through the hold; a release that's
                // too early or too late (or a press-window timeout with no
                // press at all) turns it red - both colors then hold for
                // the rest of the note's time on screen, all the way until
                // it scrolls off the bottom, matching the real outcome
                // rather than fading back to a neutral color. Unaffected by
                // lockedIn - see its own comment above for what that adds instead.
                COLORREF color = kLaneColors[lane];
                bool isHeldNote = false;
                bool isJudgedNote = false;
                bool skip = false;
                if (isLiveJudging && session.IsLaneHeld(lane) &&
                    std::abs(session.LaneHoldStartBeat(lane) - note.startBeat) < 1e-6)
                {
                    color = kNoteColorHit;
                    isHeldNote = true;
                }
                else if (isLiveJudging)
                {
                    JudgementResult result = session.OnsetJudgement(note.startBeat, lane);
                    if (result == JudgementResult::Hit)
                    {
                        color = kNoteColorHit;
                        isJudgedNote = true;
                    }
                    else if (result == JudgementResult::Miss)
                    {
                        color = kNoteColorMiss;
                        isJudgedNote = true;
                    }
                    else if (note.startBeat < session.NextExpectedBeatForLane(lane) - 1e-6)
                    {
                        // Genuinely skipped past by the judging anchor,
                        // never held or judged - only possible for a
                        // clip's very first appearance in the song, which
                        // anchors to its pattern's own true beginning (see
                        // GameSession's m_clipPatternAnchored) and so can
                        // legitimately start partway into a bar if no lane
                        // has a note exactly on beat 0. Never drawn as live.
                        skip = true;
                    }
                }
                if (skip)
                {
                    continue;
                }

                DrawNoteBar(hdc, laneX, yTop, yBottom, barHalfWidth, color, lockedIn);
                // Judged hit/miss notes get the same glow halo as a held
                // note - a plain color swap was too easy to miss at a
                // glance, so the glow makes the pass/fail moment pop.
                DrawNoteGlyph(hdc, laneX, yBottom, color, isHeldNote || isJudgedNote, lockedIn);
            }
        }
    }

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
            DrawConfetti(hdc, m_confetti, elapsed / 1000.0, t);
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
            DrawExplosion(hdc, m_explosion, elapsed / 1000.0, t);
        }
    }

    // Expanding judgement ripples (green for a hit, red for a pre-lock-in
    // miss): grow outward at each ripple's own fixed speed from that lane's
    // judge-line position, fading out continuously as they grow so the fade
    // actually reads before the ring is gone, rather than staying
    // near-opaque until it's clipped off the edge of the lane. The fade
    // (and removal) is measured against the farthest single edge of the
    // rect the ring has to cross (not the farthest corner, which is much
    // further away along the diagonal and would leave the ring looking
    // fully opaque for most of its life, fading only once it's already
    // well past the visible lane).
    for (size_t i = 0; i < m_ripples.size();)
    {
        const JudgementRipple& ripple = m_ripples[i];
        int cx = laneCenterX(ripple.lane);
        int cy = lineY;
        double elapsedSeconds = (GetTickCount() - ripple.startMs) / 1000.0;
        double radius = elapsedSeconds * ripple.speedPxPerSec;

        double maxRadius = std::max({std::abs(cx - m_laneRect.left), std::abs(cx - m_laneRect.right),
                                      std::abs(cy - m_laneRect.top), std::abs(cy - m_laneRect.bottom)});

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

    // HUD: a translucent rounded panel with bold, drop-shadowed status text
    // so it stays readable over the busy, colorful background beneath it.
    std::wstring status;
    switch (session.Phase())
    {
        case GamePhase::Idle:
            status = L"Load a chart, then Start";
            break;
        case GamePhase::CountIn:
            status = L"Get ready...";
            break;
        case GamePhase::Learning:
            if (session.CurrentSectionKind() == SectionKind::Break)
            {
                status = clip ? (clip->displayName + L" - Listen...") : L"...";
            }
            else if (clip)
            {
                status = clip->displayName;
            }
            break;
        case GamePhase::Complete:
            status = L"Song complete!";
            break;
    }

    RECT panelRect{m_laneRect.left + 8, m_laneRect.top + 8, m_laneRect.right - 8, m_laneRect.top + 44};
    DrawAlphaRoundRect(hdc, panelRect, 14, RGB(12, 8, 28), 175);

    static HFONT hudFont =
        CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hudFont);
    SetBkMode(hdc, TRANSPARENT);

    RECT textRect = panelRect;
    textRect.left += 10;
    textRect.right -= 10;

    constexpr UINT kTextFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

    // Cheap drop shadow: draw the text once in near-black offset a pixel, then again on top.
    RECT shadowRect = textRect;
    shadowRect.left += 1;
    shadowRect.top += 1;
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextW(hdc, status.c_str(), -1, &shadowRect, kTextFlags);
    SetTextColor(hdc, kTextColor);
    DrawTextW(hdc, status.c_str(), -1, &textRect, kTextFlags);

    SelectObject(hdc, oldFont);
}
