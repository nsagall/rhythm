#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "ClipColor.h"
#include "LaneConfig.h"

// Never dereferenced by a renderer (see ClipInstance::chartClip) - forward
// declared only so that field can exist as a pointer, without pulling
// ChartFile.h's actual definition into every renderer that includes this
// header.
struct ChartClip;

// The contract between NoteLaneModel (logic) and whatever draws it
// (NoteLaneRenderer.h). Everything here is beats, lane indices, and
// semantic/identity state - no pixel positions, no HDC, no GDI drawing
// calls - so a renderer can turn it into pixels however it likes, and a
// different renderer can consume the exact same struct without
// NoteLaneModel changing at all. ClipInstance::color is the one exception
// to "no colors here": a COLORREF is just a packed RGB value, not a
// drawing operation, and which one identifies a given clip is treated as
// part of that clip's identity (see ClipColor.h) rather than a rendering
// decision - every renderer gets the same, consistent answer for free.

// How far into the future/past of the pattern timeline is considered
// "current" - shared between NoteLaneModel (which notes are even
// candidates for the scene) and a renderer (how far the timeline spans on
// screen). kBeatsAhead intentionally equals kNoteFallBeats (LaneConfig.h) -
// GameSession's own lock-in advance-timing floor - so a locked-in
// section's advance always leaves a full lookahead of preview time.
constexpr double kBeatsAhead = kNoteFallBeats;
constexpr double kBeatsBehind = 1.0;

// One live playthrough of a ChartClip, for rendering purposes only - NOT
// the same object as GameSession's own private per-clip playback voice
// (audio/timing internals a renderer has no business touching). A
// ChartClip is immutable song data and can be played more than once in a
// song - a later section reusing it, or simply its own pattern looping
// again before locking in - and each such playthrough gets its own
// ClipInstance (never mutated to represent a different one), since
// lockedIn resets fresh every time and identity/color needs to be
// distinguishable even between two instances of the same ChartClip. Owned
// persistently by NoteLaneModel (see its m_previousClip/m_currentClip/
// m_nextClip) rather than rebuilt every frame - a fresh instance is only
// created when a new playthrough actually begins (or is predicted ahead of
// time - see startBeat below), not on every BuildScene call.
struct ClipInstance
{
    // Which ChartClip this is a playthrough of - this instance's own
    // identity, not something a caller needs to look up separately.
    // Never dereferenced by a renderer (only NoteLaneModel needs the song
    // data behind it); exists here purely so NoteLaneModel can tell two
    // instances of the same clip apart from a genuinely different clip
    // without an external lookup table.
    const ChartClip* chartClip = nullptr;

    // The beat this specific playthrough began (or, for a not-yet-current
    // instance, is predicted to begin) at - together with chartClip, this
    // is the instance's full identity: two instances of the same chartClip
    // with different startBeat are different playthroughs (different loop
    // repetitions), never the same one. Knowable before the playthrough is
    // actually current - a predicted loop repeat's startBeat is just the
    // current instance's own startBeat plus one spanBeats, computable the
    // instant the current instance exists, no need to wait for the loop to
    // actually happen.
    double startBeat = 0.0;

    // This clip's accent color - part of its identity (see ClipColor.h),
    // resolved once when NoteLaneModel creates this instance, so a
    // renderer never needs its own palette or index-to-color scheme just
    // to draw a clip in a consistent, recognizable color.
    COLORREF color = ClipColor::kNeutral;

    // Whether this playthrough has locked in - a renderer-chosen
    // supplementary cue (e.g. a glow outline), independent of any one
    // note's own state/color. Reset false whenever a new ClipInstance is
    // created (a fresh playthrough hasn't earned anything yet), kept in
    // sync with the session every frame while this instance is current.
    bool lockedIn = false;
};

// What a single note looks like once NoteLaneModel has resolved the
// GameSession's judging for it.
enum class NoteVisualState
{
    Normal, // not yet interacted with - a renderer colors this by clip->color
    Held,   // press judged correct, hold in progress or just resolved
    Hit,    // judged correct
    Miss,   // judged incorrect
};

// One note to draw: where and when (beats), what it should look like
// (semantically - see NoteVisualState), and which clip it belongs to -
// never a pixel position or a literal color.
struct SceneNote
{
    int lane = 0;
    double startBeat = 0.0;
    double durationBeats = 0.0;
    NoteVisualState state = NoteVisualState::Normal;

    // Never null for a note that actually made it into NoteLaneScene::
    // notes/explodingNotes - points into one of NoteLaneModel's own
    // persistent instances (see ClipInstance's own comment), which outlive
    // this NoteLaneScene, so the pointer stays valid for at least as long
    // as this NoteLaneScene does.
    const ClipInstance* clip = nullptr;
};

// One lane's judge-line receptor state, as far as game rules go - a
// renderer adds its own transient visual feedback (flash/ripple) on top,
// driven separately by INoteLaneRenderer::OnJudgement.
struct SceneReceptor
{
    bool held = false;
};

// Everything NoteLaneModel resolved from one GameSession for a single
// frame.
struct NoteLaneScene
{
    bool clockRunning = false;
    double nowBeat = 0.0;
    int beatsPerBar = 4;

    // Rails/receptors' base color/identity - nullptr means no current/
    // preview clip (Idle or Complete), which a renderer should show as a
    // neutral color.
    const ClipInstance* primaryClip = nullptr;

    SceneReceptor receptors[kLaneCount];
    std::vector<SceneNote> notes;

    // Edge-triggered this frame only (never stays true across frames) -
    // lets a renderer react once, the instant either happens, without
    // tracking its own copy of the session's lock-in/handoff state.
    bool justLockedIn = false;
    bool justHandedOff = false;
    // Candidate notes for whichever of the above fired this frame (may be
    // both at once, and the two triggers' notes are not distinguished from
    // each other since they're drawn identically) - a renderer reacting to
    // these should still apply its own on-screen visibility check first,
    // exactly as it would for NoteLaneScene::notes.
    std::vector<SceneNote> explodingNotes;

    std::wstring statusText;

    // Which ChartClip (by its .chart name, not displayName) each of NoteLaneModel's own
    // m_previousClip/m_currentClip/m_nextClip currently identifies, or
    // L"(none)" for a null slot - always populated (cheap; a handful of
    // pointer derefs), same as statusText, regardless of whether any
    // renderer actually draws it. Purely a debugging aid for the
    // previous/current/next instance chain itself; a renderer decides for
    // itself whether/when to show it (see INoteLaneRenderer::
    // ToggleDebugOverlay).
    std::wstring debugPreviousClipName;
    std::wstring debugCurrentClipName;
    std::wstring debugNextClipName;
};
