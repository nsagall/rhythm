#pragma once

#include <unordered_map>
#include <vector>
#include <windows.h>

#include "NoteLaneRenderer.h"  // INoteLaneRenderer is the base class.
#include "NoteLaneScene.h"     // NoteVisualState enum used by value in ColorForNote.

// GDI implementation of INoteLaneRenderer: layout math (beats/lanes -> pixels), every drawing call,
// resource caching (brushes/pens/scratch alpha-blend buffer/HUD font), and every continuous
// animation/particle system (confetti, explosion sparks, judgement ripples, press/release flash,
// background sparkles).
class NoteLaneGdiRenderer : public INoteLaneRenderer
{
public:
    ~NoteLaneGdiRenderer() override;

    void OnJudgement(JudgementResult result, int lane, bool passing, bool precise) override;
    void OnHudValueChanged(GameSession::HudField field, int newValue) override;
    void Draw(HDC hdc, RECT laneRect, RECT hitsMeterRect, const NoteLaneScene& scene) override;
    void ToggleDebugOverlay() override;

private:
    // One piece of confetti spawned when a track locks in: falls under gravity with horizontal
    // drift, fading over a fixed lifetime. Simulated purely from elapsed time in DrawConfetti.
    struct ConfettiPiece
    {
        double x = 0.0;
        double y = 0.0;
        double velX = 0.0;
        double velY = 0.0;
        double spinPhase = 0.0;
        COLORREF color = 0;
    };

    // One spark from the burst spawned when a note in scene.explodingNotes reacts to a
    // lock-in/handoff: flies outward with drag from where that note was, fading over a fixed lifetime.
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

    // A "-N" popup spawned when the bank drops to 0 from nonzero (a streak trip): sinks and shakes
    // while fading. Carries no baked-in pixel position - DrawScorePopups resolves it against the
    // HUD panel rect at draw time, so it survives a window resize mid-animation.
    struct ScorePopup
    {
        DWORD startMs = 0;
        int amount = 0;
    };

    // Fakes per-primitive transparency (GDI has none): captures the region under its bounds into
    // the scratch buffer, draws the shape solid on the copy, then blends the scratch rect back.
    // scratchDc must already be at least bounds-sized (see EnsureScratchBuffer).
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

    // Pixel-space layout, recomputed from laneRect each call rather than cached, so every drawing
    // method stays layout-stateless.
    double PixelsPerBeat(RECT laneRect) const;
    int LineY(RECT laneRect) const;
    int LaneCenterX(RECT laneRect, int lane) const;
    int YForBeatsFromNow(RECT laneRect, double beatsFromNow) const;

    // Returns clipColor for a Normal note, or the fixed held/hit/miss color
    // for anything else - clipColor comes from ClipPlaythrough::color (see
    // ClipColor.h), never from a palette of this renderer's own. precise is
    // only consulted for state == Hit - see c_NoteColorHitImprecise.
    COLORREF ColorForNote(NoteVisualState state, COLORREF clipColor, bool precise) const;

    HBRUSH CachedSolidBrush(COLORREF color);
    HPEN CachedSolidPen(int width, COLORREF color);
    // Returns a scratch compatible DC at least width x height, shared by every AlphaShape (grown,
    // never shrunk) - safe because AlphaShapes are used one at a time, never nested.
    HDC EnsureScratchBuffer(HDC referenceHdc, int width, int height);

    void DrawAlphaCircle(HDC hdc, int cx, int cy, int radius, COLORREF color, BYTE alpha);
    void DrawAlphaRing(HDC hdc, int cx, int cy, int radius, int thickness, COLORREF color, BYTE alpha);
    void DrawAlphaRoundRect(HDC hdc, RECT rect, int cornerRadius, COLORREF color, BYTE alpha);
    // Same as DrawAlphaRoundRect but an outline only (thickness px wide) -
    // used for the HUD panel's brief green/red "points just moved" glow, so
    // it reads as a pulsing border rather than a filled wash over the text.
    void DrawAlphaRoundRectOutline(HDC hdc, RECT rect, int cornerRadius, int thickness, COLORREF color, BYTE alpha);
    void DrawAlphaRect(HDC hdc, int cx, int cy, int halfWidth, int halfHeight, COLORREF color, BYTE alpha);
    // Alpha-blends text drawn with DrawTextW - same capture/draw/blend trick
    // as the other DrawAlpha* helpers, so a popup or pulsing readout can
    // fade smoothly instead of only being able to hard-cut to invisible.
    // No-op for empty text (callers never need to special-case that).
    void DrawAlphaText(HDC hdc, RECT rect, const std::wstring& text, HFONT font, COLORREF color, UINT flags,
                        BYTE alpha);

    void DrawNoteGlyph(HDC hdc, int x, int y, COLORREF color, bool glow, bool passing);
    void DrawNoteBar(HDC hdc, int x, int yTop, int yBottom, int halfWidth, COLORREF color, bool passing);
    void DrawReceptor(HDC hdc, int x, int y, COLORREF laneColor, bool held, bool flashing, COLORREF flashColor,
                       double flashProgress);
    void DrawSparkles(HDC hdc, RECT laneRect, double beatPulse);
    void DrawMeasureLines(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    // beatPulse (see Draw()'s own comment) drives how squiggly the rail
    // currently is - a straight line between beats, bulging into a sine
    // wave right on each beat and settling back as the pulse decays.
    void DrawRails(HDC hdc, RECT laneRect, COLORREF primaryColor, double beatPulse);
    void DrawReceptors(HDC hdc, RECT laneRect, const NoteLaneScene& scene, COLORREF primaryColor);
    void DrawNotes(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    void DrawConfetti(HDC hdc, double elapsedSeconds, double t);
    void DrawExplosion(HDC hdc, double elapsedSeconds, double t);
    void DrawRipples(HDC hdc, RECT laneRect);
    void DrawHud(HDC hdc, RECT laneRect, const std::wstring& statusText, const std::wstring& scoreText,
                 const std::wstring& bankText, const std::wstring& multiplierText);
    // Returns baseFont once growUntilMs has passed; otherwise (re)creates growFontSlot at a size
    // interpolated from basePointSize*2 down to basePointSize as growUntilMs approaches, and
    // returns that. growFontSlot is a per-HUD-value cached temporary font, recreated each frame
    // during that value's brief grow window.
    HFONT FontForGrowPulse(HFONT baseFont, int basePointSize, DWORD growUntilMs, HFONT& growFontSlot, DWORD now);
    // Drops expired m_scorePopups entries, then draws the rest anchored under panelRect. Called
    // from DrawHud after the panel/text, so popups float on top.
    void DrawScorePopups(HDC hdc, RECT panelRect);
    // Only called when m_debugOverlayEnabled - see ToggleDebugOverlay.
    void DrawDebugOverlay(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    // The "hits meter" panel beside the playfield - a bottom-anchored fill tracking
    // scene.hitsMeterProgress. Draws nothing while scene.showHitsMeter is false. beatPulse
    // brightens the fill on the beat while scene.hitsMeterPulsing is true.
    void DrawHitsMeter(HDC hdc, RECT hitsMeterRect, const NoteLaneScene& scene, double beatPulse);

    // Spawns a lock-in confetti burst across laneRect's width.
    void SpawnConfetti(RECT laneRect);
    // Spawns an explosion burst for each of scene's exploding notes that's
    // actually within laneRect right now.
    void SpawnExplosion(RECT laneRect, const NoteLaneScene& scene);
    // Adds a spark burst at hitsMeterRect's center into m_explosion (sharing its timer). Called
    // alongside SpawnExplosion, only on scene.justLockedIn for a DontFail clip.
    void AppendHitsMeterExplosion(RECT hitsMeterRect, COLORREF color);
    // Adds a small confetti burst across hitsMeterRect's width into m_confetti (sharing its timer).
    // Called alongside SpawnConfetti, only on scene.justLockedIn for a Pass clip - its one lock-in
    // for the section.
    void AppendHitsMeterConfetti(RECT hitsMeterRect);

    std::unordered_map<COLORREF, HBRUSH> m_brushCache;
    std::unordered_map<UINT64, HPEN> m_penCache;

    HDC m_scratchDc = nullptr;
    HBITMAP m_scratchBitmap = nullptr;
    HBITMAP m_scratchDefaultBitmap = nullptr;
    int m_scratchWidth = 0;
    int m_scratchHeight = 0;

    HFONT m_hudFont = nullptr;
    // Smaller than m_hudFont - used for the pending-score readout and score
    // popups, so neither competes with the main status/score line for
    // attention.
    HFONT m_smallHudFont = nullptr;

    // See ToggleDebugOverlay/DrawDebugOverlay.
    bool m_debugOverlayEnabled = false;

    JudgementResult m_flashResult[c_LaneCount] = {};
    DWORD m_flashUntilMs[c_LaneCount] = {};
    std::vector<JudgementRipple> m_ripples;

    // See OnHudValueChanged/DrawScorePopups. m_scoreFlashUntilMs drives the HUD panel's brief red
    // glow on a bank wipe. m_scorePopups is uncapped - at most one is ever alive at once.
    std::vector<ScorePopup> m_scorePopups;
    DWORD m_scoreFlashUntilMs = 0;

    // Bank's value as of the last OnHudValueChanged(Bank, ...) call - lets that call tell an
    // ordinary increase apart from a streak-trip drop to 0.
    int m_lastBankValue = 0;

    // See FontForGrowPulse. One grow deadline + one cached temporary font per HUD value.
    DWORD m_totalGrowUntilMs = 0;
    DWORD m_bankGrowUntilMs = 0;
    DWORD m_multiplierGrowUntilMs = 0;
    HFONT m_totalGrowFont = nullptr;
    HFONT m_bankGrowFont = nullptr;
    HFONT m_multiplierGrowFont = nullptr;

    DWORD m_confettiStartMs = 0;
    std::vector<ConfettiPiece> m_confetti;

    DWORD m_explosionStartMs = 0;
    std::vector<ExplosionParticle> m_explosion;
};
