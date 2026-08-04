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
    virtual void OnJudgement(JudgementResult result, int lane, bool lockedIn) = 0;

    // Draws one frame of scene into laneRect on hdc, and advances any of
    // the renderer's own continuous animations by however much wall-clock
    // time has passed since its last call.
    virtual void Draw(HDC hdc, RECT laneRect, const NoteLaneScene& scene) = 0;
};
