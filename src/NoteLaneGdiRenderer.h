#pragma once

#include <unordered_map>
#include <vector>
#include <windows.h>

#include "NoteLaneRenderer.h"

// GDI implementation of INoteLaneRenderer: layout math (beats/lanes ->
// pixels), every actual drawing call, resource caching (brushes/pens/a
// scratch alpha-blend buffer/the HUD font), and every continuous
// animation/particle system (confetti, explosion sparks, judgement
// ripples, press/release flash, background sparkles) - none of which
// NoteLaneModel or NoteLane know anything about.
class NoteLaneGdiRenderer : public INoteLaneRenderer
{
public:
    ~NoteLaneGdiRenderer() override;

    void OnJudgement(JudgementResult result, int lane, bool lockedIn) override;
    void Draw(HDC hdc, RECT laneRect, const NoteLaneScene& scene) override;

private:
    // One piece of confetti spawned when a track locks in: falls from its
    // starting position under simple gravity with a bit of horizontal
    // drift, fading out over its fixed lifetime. Positions/colors are
    // generated once at spawn time and simulated purely from elapsed time
    // in DrawConfetti.
    struct ConfettiPiece
    {
        double x = 0.0;
        double y = 0.0;
        double velX = 0.0;
        double velY = 0.0;
        double spinPhase = 0.0;
        COLORREF color = 0;
    };

    // One spark from the notes-exploding burst spawned when a note in
    // scene.explodingNotes reacts to a lock-in/handoff: flies straight
    // outward (with drag, so it decelerates rather than flying off
    // forever) from wherever that note was, fading out over its fixed
    // lifetime.
    struct ExplosionParticle
    {
        double x = 0.0;
        double y = 0.0;
        double velX = 0.0;
        double velY = 0.0;
        COLORREF color = 0;
    };

    // An expanding colored ring spawned at the instant OnJudgement is
    // called - starts at that lane's judge-line position and grows
    // outward until it's fully left the visible lane.
    struct JudgementRipple
    {
        int lane = 0;
        DWORD startMs = 0;
        COLORREF color = 0;
        double speedPxPerSec = 0.0;
        int startAlpha = 0;
        double fadeRate = 1.0;
    };

    // Alpha-blends a filled shape over whatever is already at its bounds,
    // by capturing that region into the renderer's scratch buffer first,
    // drawing the shape solid on top of the copy, then blending the
    // scratch rect back - GDI has no real per-primitive transparency, so
    // this fakes it. scratchDc must already be at least bounds-sized (see
    // EnsureScratchBuffer).
    class AlphaShape
    {
    public:
        AlphaShape(HDC destHdc, RECT bounds, HDC scratchDc);
        HDC Dc() const
        {
            return m_memHdc;
        }
        void Blend(BYTE alpha) const;

    private:
        HDC m_destHdc = nullptr;
        RECT m_bounds{};
        int m_width = 0;
        int m_height = 0;
        HDC m_memHdc = nullptr;
    };

    // Pixel-space layout, recomputed from laneRect on every call rather
    // than cached - cheap arithmetic, and it keeps every drawing method
    // stateless with respect to layout instead of threading extra fields
    // through the object between Draw() calls.
    double PixelsPerBeat(RECT laneRect) const;
    int LineY(RECT laneRect) const;
    int LaneCenterX(RECT laneRect, int lane) const;
    int YForBeatsFromNow(RECT laneRect, double beatsFromNow) const;

    COLORREF ColorForClip(int clipIndex) const;
    COLORREF ColorForNote(NoteVisualState state, int clipIndex) const;

    HBRUSH CachedSolidBrush(COLORREF color);
    HPEN CachedSolidPen(int width, COLORREF color);
    // Returns a scratch compatible DC at least width x height, shared by
    // every AlphaShape use (grown, never shrunk) - safe since AlphaShape
    // instances are always fully used up (constructed, drawn into,
    // blended) one at a time before the next is created; Draw() never
    // nests two of them.
    HDC EnsureScratchBuffer(HDC referenceHdc, int width, int height);

    void DrawAlphaCircle(HDC hdc, int cx, int cy, int radius, COLORREF color, BYTE alpha);
    void DrawAlphaRing(HDC hdc, int cx, int cy, int radius, int thickness, COLORREF color, BYTE alpha);
    void DrawAlphaRoundRect(HDC hdc, RECT rect, int cornerRadius, COLORREF color, BYTE alpha);
    void DrawAlphaRect(HDC hdc, int cx, int cy, int halfWidth, int halfHeight, COLORREF color, BYTE alpha);

    void DrawNoteGlyph(HDC hdc, int x, int y, COLORREF color, bool glow, bool lockedIn);
    void DrawNoteBar(HDC hdc, int x, int yTop, int yBottom, int halfWidth, COLORREF color, bool lockedIn);
    void DrawReceptor(HDC hdc, int x, int y, COLORREF laneColor, bool held, bool flashing, COLORREF flashColor,
                       double flashProgress);
    void DrawSparkles(HDC hdc, RECT laneRect, double beatPulse);
    void DrawMeasureLines(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    void DrawRails(HDC hdc, RECT laneRect, COLORREF primaryColor);
    void DrawReceptors(HDC hdc, RECT laneRect, const NoteLaneScene& scene, COLORREF primaryColor);
    void DrawNotes(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    void DrawConfetti(HDC hdc, double elapsedSeconds, double t);
    void DrawExplosion(HDC hdc, double elapsedSeconds, double t);
    void DrawRipples(HDC hdc, RECT laneRect);
    void DrawHud(HDC hdc, RECT laneRect, const std::wstring& statusText);

    // Spawns a lock-in confetti burst across laneRect's width.
    void SpawnConfetti(RECT laneRect);
    // Spawns an explosion burst for each of scene's exploding notes that's
    // actually within laneRect right now.
    void SpawnExplosion(RECT laneRect, const NoteLaneScene& scene);

    std::unordered_map<COLORREF, HBRUSH> m_brushCache;
    std::unordered_map<UINT64, HPEN> m_penCache;

    HDC m_scratchDc = nullptr;
    HBITMAP m_scratchBitmap = nullptr;
    HBITMAP m_scratchDefaultBitmap = nullptr;
    int m_scratchWidth = 0;
    int m_scratchHeight = 0;

    HFONT m_hudFont = nullptr;

    JudgementResult m_flashResult[kLaneCount] = {};
    DWORD m_flashUntilMs[kLaneCount] = {};
    std::vector<JudgementRipple> m_ripples;

    DWORD m_confettiStartMs = 0;
    std::vector<ConfettiPiece> m_confetti;

    DWORD m_explosionStartMs = 0;
    std::vector<ExplosionParticle> m_explosion;
};
