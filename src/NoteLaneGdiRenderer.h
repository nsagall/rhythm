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

    void OnJudgement(JudgementResult result, int lane, bool passing, bool precise) override;
    void OnHudValueChanged(GameSession::HudField field, int newValue) override;
    void Draw(HDC hdc, RECT laneRect, RECT hitsMeterRect, const NoteLaneScene& scene) override;
    void ToggleDebugOverlay() override;

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

    // A "-N" popup spawned by OnHudValueChanged the instant the bank drops
    // to exactly 0 from a nonzero value (a streak trip wiping it - see
    // GameSession::HudChangeEvent): sinks and shakes while fading, reading
    // as the points crumbling away. Like JudgementRipple, carries no
    // baked-in pixel position - DrawScorePopups resolves it against the HUD
    // panel rect at draw time, purely as a function of elapsed time since
    // startMs, so it stays correct across a window resize mid-animation.
    struct ScorePopup
    {
        DWORD startMs = 0;
        int amount = 0;
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

    // Returns clipColor for a Normal note, or the fixed held/hit/miss color
    // for anything else - clipColor comes from ClipInstance::color (see
    // ClipColor.h), never from a palette of this renderer's own. precise is
    // only consulted for state == Hit - see kNoteColorHitImprecise.
    COLORREF ColorForNote(NoteVisualState state, COLORREF clipColor, bool precise) const;

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
    // Returns baseFont unchanged once growUntilMs has passed; otherwise
    // (re)creates growFontSlot at a point size interpolated from
    // basePointSize*2 (right when OnHudValueChanged triggered it) down to
    // basePointSize (as growUntilMs is reached) and returns that instead -
    // see kHudGrowDurationMs. growFontSlot is a specific field's own cached
    // temporary font (m_totalGrowFont/m_bankGrowFont/m_multiplierGrowFont),
    // recreated on essentially every animating frame since the interpolated
    // size is different each time - cheap, and only happens during each
    // value's own brief ~1s window.
    HFONT FontForGrowPulse(HFONT baseFont, int basePointSize, DWORD growUntilMs, HFONT& growFontSlot, DWORD now);
    // Drops any expired entry from m_scorePopups, then draws whatever's
    // left anchored under panelRect (the HUD panel's own rect) - called
    // once per Draw() from DrawHud, after the panel/text themselves, so
    // popups float over top of everything else in the panel.
    void DrawScorePopups(HDC hdc, RECT panelRect);
    // Only called when m_debugOverlayEnabled - see ToggleDebugOverlay.
    void DrawDebugOverlay(HDC hdc, RECT laneRect, const NoteLaneScene& scene);
    // The "hits meter" panel beside the playfield - a bottom-anchored fill
    // tracking scene.hitsMeterProgress. Draws nothing while
    // scene.showHitsMeter is false. beatPulse (see Draw()'s own comment) -
    // brightens the fill gently on the beat while scene.hitsMeterPulsing is
    // true (a locked-in Pass section holding its bar full).
    void DrawHitsMeter(HDC hdc, RECT hitsMeterRect, const NoteLaneScene& scene, double beatPulse);

    // Spawns a lock-in confetti burst across laneRect's width.
    void SpawnConfetti(RECT laneRect);
    // Spawns an explosion burst for each of scene's exploding notes that's
    // actually within laneRect right now.
    void SpawnExplosion(RECT laneRect, const NoteLaneScene& scene);
    // Adds a burst of sparks at hitsMeterRect's own center into the same
    // m_explosion vector (and sharing its timer) that SpawnExplosion just
    // populated - called alongside it, only on scene.justLockedIn for a
    // DontFail clip (see NoteLaneScene::hitsMeterIsDontFail), so the hits
    // meter's disappearance gets its own little celebration instead of just
    // vanishing the instant showHitsMeter flips false.
    void AppendHitsMeterExplosion(RECT hitsMeterRect, COLORREF color);
    // Adds a small confetti burst scattered across hitsMeterRect's own
    // width into the same m_confetti vector (and sharing its timer) that
    // SpawnConfetti just populated - called alongside it, only on
    // scene.justLockedIn for a Pass clip (see NoteLaneScene::
    // hitsMeterIsDontFail), since that's the meter's one and only
    // disappearance for the whole section (passing is a one-way latch),
    // unlike DontFail's own AppendHitsMeterExplosion above.
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

    JudgementResult m_flashResult[kLaneCount] = {};
    DWORD m_flashUntilMs[kLaneCount] = {};
    std::vector<JudgementRipple> m_ripples;

    // See OnHudValueChanged/DrawScorePopups. m_scoreFlashUntilMs drives the
    // HUD panel's own brief red glow on a bank wipe, separate from the
    // popup's own lifetime - m_scorePopups itself has no cap, since in
    // practice at most one is ever alive at once (a wipe is comparatively
    // rare, unlike the old per-hit Banked event this replaces).
    std::vector<ScorePopup> m_scorePopups;
    DWORD m_scoreFlashUntilMs = 0;

    // Bank's own value as of the last OnHudValueChanged(Bank, ...) call -
    // lets that call tell an ordinary increase apart from the drop-to-0 that
    // means a streak trip just wiped it (see ScorePopup's own comment),
    // without GameSession needing to describe *why* the value changed.
    int m_lastBankValue = 0;

    // See FontForGrowPulse. One growUntilMs deadline + one cached temporary
    // font per HUD value (GameSession::HudField), set by OnHudValueChanged
    // and consumed by DrawHud.
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
