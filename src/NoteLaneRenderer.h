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

    // Notifies the renderer that amount points just moved permanently into
    // the banked total (GameSession::ScoreEvent::Kind::Banked) - a "happy"
    // flourish as they land, distinct from the ordinary Hit ripple that
    // already fired for whichever press/release actually earned them.
    // amount is always > 0 - see GameSession::ConsumeScoreEvents.
    virtual void OnScoreBanked(int amount) = 0;

    // Notifies the renderer that amount not-yet-banked points just vanished
    // to a real miss (GameSession::ScoreEvent::Kind::Lost) - a "sad"
    // flourish as they disappear, distinct from the ordinary Miss ripple
    // that already fired for the same press/release/timeout. amount is
    // always > 0 - see GameSession::ConsumeScoreEvents.
    virtual void OnScoreLost(int amount) = 0;

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
