#pragma once

#include <windows.h>

#include "GameSession.h"
#include "NoteLaneScene.h"

// The swap point for a different visual style: anything implementing this
// can replace NoteLaneGdiRenderer with zero changes to NoteLaneModel or
// NoteLane, since neither one knows a concrete renderer exists - see
// NoteLane.h's m_renderer. Owns every bit of continuous animation/particle
// state (confetti, explosion, ripples, flash) itself, since that's purely
// a visual choice, not a game rule.
class INoteLaneRenderer
{
public:
    virtual ~INoteLaneRenderer() = default;

    // Notifies the renderer of a fresh press/release judgement, for
    // whatever transient visual feedback it wants to show - a flash, a
    // ripple, nothing at all. Purely cosmetic; never affects scoring.
    // precise mirrors GameSession::JudgementEvent::precise - meaningful only
    // when result == Hit - so a hit whose press wasn't precise can be shown
    // differently (see NoteLaneGdiRenderer's yellow-vs-green ripple).
    virtual void OnJudgement(JudgementResult result, int lane, bool passing, bool precise) = 0;

    // Notifies the renderer that one of the three HUD values (total score/
    // bank/multiplier - see GameSession::HudField/ConsumeHudChangeEvents)
    // just changed to newValue, so it can grow that value's own on-screen
    // text 2x for about a second. newValue lets the renderer tell a Bank
    // field's ordinary per-hit increase apart from the moment it drops to
    // exactly 0 (a streak trip wiping it), for its own "streak broken"
    // flourish - see GameSession::HudChangeEvent's own comment.
    virtual void OnHudValueChanged(GameSession::HudField field, int newValue) = 0;

    // Draws one frame of scene into laneRect on hdc - plus, beside it,
    // hitsMeterRect (see NoteLaneScene::showHitsMeter/hitsMeterProgress) -
    // and advances any of the renderer's own continuous animations by
    // however much wall-clock time has passed since its last call.
    virtual void Draw(HDC hdc, RECT laneRect, RECT hitsMeterRect, const NoteLaneScene& scene) = 0;

    // Flips whether Draw() also shows scene's debugPreviousClipName/
    // debugCurrentClipName/debugNextClipName - off by default. Purely a
    // display choice (which is exactly why this lives on the renderer, not
    // NoteLaneModel/NoteLaneScene - the data itself is always populated
    // regardless of this toggle).
    virtual void ToggleDebugOverlay() = 0;
};
