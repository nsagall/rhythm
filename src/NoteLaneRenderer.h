#pragma once

#include <windows.h>

#include "GameSession.h"  // JudgementResult and GameSession::HudField enums are used by value below.

struct NoteLaneScene;

// The swap point for a different visual style: anything implementing this replaces
// NoteLaneGdiRenderer with zero changes to NoteLaneModel or NoteLane, neither of which knows a
// concrete renderer exists. Owns every bit of continuous animation/particle state (confetti,
// explosion, ripples, flash), since that's a visual choice, not a game rule.
class INoteLaneRenderer
{
public:
    virtual ~INoteLaneRenderer() = default;

    // Notifies the renderer of a fresh press/release judgement, for whatever transient feedback it
    // wants (a flash, a ripple, nothing). Purely cosmetic.
    //   result  - the judgement.
    //   lane    - the lane it was judged on.
    //   passing - whether the section is passing.
    //   precise - mirrors GameSession::JudgementEvent::precise; meaningful only when result == Hit.
    virtual void OnJudgement(JudgementResult result, int lane, bool passing, bool precise) = 0;

    // Notifies the renderer that a HUD value just changed, so it can grow that value's text
    // briefly.
    //   field    - which HUD value (total score / bank / multiplier).
    //   newValue - the new value; lets the renderer tell a Bank increase apart from a streak trip
    //              wiping it to 0.
    virtual void OnHudValueChanged(GameSession::HudField field, int newValue) = 0;

    // Draws one frame of scene into laneRect on hdc, plus hitsMeterRect beside it, and advances the
    // renderer's own continuous animations by the wall-clock time since its last call.
    virtual void Draw(HDC hdc, RECT laneRect, RECT hitsMeterRect, const NoteLaneScene& scene) = 0;

    // Flips whether Draw() also shows scene's debug clip names - off by default. The scene data is
    // always populated regardless of this toggle.
    virtual void ToggleDebugOverlay() = 0;
};
