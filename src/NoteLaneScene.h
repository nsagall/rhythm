#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "Colors.h"
#include "LaneConfig.h"

// Forward declared only so ClipPlaythrough::chartClip can be a pointer without pulling ChartClip.h
// into every renderer. Never dereferenced by a renderer.
class ChartClip;

// The contract between NoteLaneModel (logic) and whatever draws it (NoteLaneRenderer.h). Everything
// here is beats, lane indices, and semantic/identity state - no pixel positions, no HDC, no drawing
// calls - so a different renderer can consume the same struct without NoteLaneModel changing.
// ClipPlaythrough::color is the one exception to "no colors": a COLORREF is a packed RGB value, and
// which one identifies a clip is part of that clip's identity (see ClipColor.h), not a rendering choice.

// How far into the future/past of the pattern timeline counts as "current" - shared between
// NoteLaneModel (which notes are scene candidates) and a renderer (how far the timeline spans on
// screen). c_BeatsAhead equals c_NoteFallBeats so a locked-in section's advance always leaves a
// full lookahead of preview time.
constexpr double c_BeatsAhead = c_NoteFallBeats;
constexpr double c_BeatsBehind = 1.0;

// One live playthrough of a ChartClip, for rendering only - NOT GameSession's private per-clip
// playback voice. A ChartClip is immutable and can play more than once in a song (a later section
// reusing it, or its pattern looping again before lock-in); each playthrough gets its own
// ClipPlaythrough, since passing resets fresh each time and identity/color must stay distinguishable
// between two instances of the same ChartClip. Owned persistently by NoteLaneModel, not rebuilt
// every frame - created only when a new playthrough begins or is predicted (see startBeat).
struct ClipPlaythrough
{
    // Which ChartClip this is a playthrough of - this instance's identity. Never dereferenced by a
    // renderer; here so NoteLaneModel can tell two instances of the same clip apart without a lookup table.
    const ChartClip* chartClip = nullptr;

    // The beat this playthrough began (or, for a predicted instance, is predicted to begin) at.
    // With chartClip, this is the instance's full identity: same chartClip + different startBeat =
    // different playthroughs. A predicted loop repeat's startBeat is the current instance's
    // startBeat plus one spanBeats, computable before the loop happens.
    double startBeat = 0.0;

    // This clip's accent color - part of its identity (see ClipColor.h), resolved once when
    // NoteLaneModel creates this instance, so a renderer needs no palette of its own.
    COLORREF color = ClipColor::c_Neutral;

    // Whether this playthrough is currently passing, for a renderer's "correct" glow cue. Reset
    // false when the instance is created, kept in sync with the session every frame while current -
    // except a DontFail clip, which never sets this true (it conveys progress through the hits
    // meter instead of a glow that would flicker on every miss/recovery). In Pass mode this goes
    // false->true once.
    bool passing = false;
};

// What a single note looks like once NoteLaneModel has resolved the GameSession's judging for it.
enum class NoteVisualState
{
    Normal, // Not yet interacted with - a renderer colors this by clip->color.
    Held,   // Press judged correct, hold in progress or just resolved.
    Hit,    // Judged correct.
    Miss,   // Judged incorrect.
};

// One note to draw: where and when (beats), what it should look like (semantically), and which clip
// it belongs to - never a pixel position or a literal color.
struct SceneNote
{
    int lane = 0;
    double startBeat = 0.0;
    double durationBeats = 0.0;
    NoteVisualState state = NoteVisualState::Normal;

    // Only meaningful when state == Hit - mirrors GameSession::OnsetPrecise. False for a correct
    // press outside half the tolerance window (still a hit, worth less; a renderer colors it
    // differently - see NoteLaneGdiRenderer::ColorForNote).
    bool precise = true;

    // Never null for a note that made it into NoteLaneScene::notes/explodingNotes - points into one
    // of NoteLaneModel's persistent instances, which outlive this NoteLaneScene.
    const ClipPlaythrough* clip = nullptr;
};

// One lane's judge-line receptor state as far as game rules go - a renderer adds its own transient
// feedback (flash/ripple) on top, driven by INoteLaneRenderer::OnJudgement.
struct SceneReceptor
{
    bool held = false;
};

// Everything NoteLaneModel resolved from one GameSession for a single frame.
struct NoteLaneScene
{
    bool clockRunning = false;
    double nowBeat = 0.0;
    int beatsPerBar = 4;

    // Rails/receptors' base color/identity - nullptr means no current/preview clip (Idle or
    // Complete), which a renderer should show as neutral.
    const ClipPlaythrough* primaryClip = nullptr;

    SceneReceptor receptors[c_LaneCount];
    std::vector<SceneNote> notes;

    // Edge-triggered this frame only - lets a renderer react once without tracking its own copy of
    // the session's passing/handoff state.
    bool justLockedIn = false;
    bool justHandedOff = false;
    // DontFail mode only: the current clip was passing and just reverted to failing. Never true in
    // Pass mode (a one-way latch). The reverse direction (failing -> passing) is justLockedIn.
    bool justFailed = false;
    // Candidate notes for whichever trigger fired this frame (possibly more than one; not
    // distinguished, since they draw identically). A renderer should still apply its own on-screen
    // visibility check, as for notes.
    std::vector<SceneNote> explodingNotes;

    // Progress for the "hits meter" panel, clamped to [0,1] - only meaningful while showHitsMeter,
    // and means two different things by hitsMeterIsDontFail:
    //   - Pass mode: streak / hitsRequired ("how close to locking in"), reaching 1.0 at lock-in and
    //     staying pinned there for the rest of the section - see hitsMeterPulsing.
    //   - DontFail mode: how far nowBeat has gotten through the clip's current loop repetition
    //     ("how far through the clip"), live while passing, frozen when a miss drops to failing,
    //     jumping to the live value on re-lock-in, and reset to 0 if a whole loop elapses while
    //     still failing. See NoteLaneModel::BuildScene for the implementation.
    double hitsMeterProgress = 0.0;

    // True whenever the hits meter should be visible - the entire duration of a current learn
    // section in both modes, including after a Pass section locks in. False once the section
    // changes to anything else.
    bool showHitsMeter = false;

    // Pass mode only: true once a Pass-mode section has locked in, for the rest of that section's
    // run (mirrors IsPassing(); hitsMeterProgress is pinned at 1.0). A renderer should hold the bar
    // full and pulse it to the beat - see NoteLaneGdiRenderer::DrawHitsMeter. Always false for DontFail.
    bool hitsMeterPulsing = false;

    // Which lock-in treatment the hits meter gets on a justLockedIn frame: true for DontFail (a
    // small spark burst - the meter can lock in many times per section), false for Pass (a confetti
    // burst as the meter settles into its held-full state - the section's one lock-in). Only read
    // on a justLockedIn frame, but populated any time there's a current learn clip.
    bool hitsMeterIsDontFail = false;

    std::wstring statusText;

    // The permanent running total, formatted (e.g. L"Score 12,340") - see GameSession::CurrentScore().
    // Drawn by INoteLaneRenderer::DrawHud alongside statusText.
    std::wstring scoreText;

    // The bank's not-yet-paid-out amount, formatted (e.g. L"+120") - see GameSession::CurrentBank().
    // Empty when 0. Meant to read as points building up, separate from scoreText.
    std::wstring bankText;

    // The multiplier CurrentBank() will pay out at now (e.g. L"x2") - see GameSession::CurrentMultiplier().
    // Empty at the base x1 rate.
    std::wstring multiplierText;

    // Which ChartClip (by .chart name) each of NoteLaneModel's m_previousClip/m_currentClip/
    // m_nextClip identifies, or L"(none)". Always populated; purely a debugging aid a renderer
    // shows via INoteLaneRenderer::ToggleDebugOverlay.
    std::wstring debugPreviousClipName;
    std::wstring debugCurrentClipName;
    std::wstring debugNextClipName;
};
