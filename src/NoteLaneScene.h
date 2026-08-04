#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "LaneConfig.h"

// The contract between NoteLaneModel (logic) and whatever draws it
// (NoteLaneRenderer.h). Everything here is beats, lane indices, and
// semantic state - no pixel positions, no HDC, no GDI - so a renderer can
// turn it into pixels however it likes, and a different renderer can
// consume the exact same struct without NoteLaneModel changing at all.

// How far into the future/past of the pattern timeline is considered
// "current" - shared between NoteLaneModel (which notes are even
// candidates for the scene) and a renderer (how far the timeline spans on
// screen). kBeatsAhead intentionally equals kNoteFallBeats (LaneConfig.h) -
// GameSession's own lock-in advance-timing floor - so a locked-in
// section's advance always leaves a full lookahead of preview time.
constexpr double kBeatsAhead = kNoteFallBeats;
constexpr double kBeatsBehind = 1.0;

// What a single note looks like once NoteLaneModel has resolved the
// GameSession's judging for it.
enum class NoteVisualState
{
    Normal, // not yet interacted with - a renderer colors this by instrumentIndex
    Held,   // press judged correct, hold in progress or just resolved
    Hit,    // judged correct
    Miss,   // judged incorrect
};

// One note to draw: where and when (beats), and what it should look like
// (semantically - see NoteVisualState) - never a pixel position.
struct SceneNote
{
    int lane = 0;
    double startBeat = 0.0;
    double durationBeats = 0.0;
    NoteVisualState state = NoteVisualState::Normal;
    // This note's clip's stable position in ChartSong::clips, or -1 for
    // none - a renderer resolves this to an actual color via its own
    // palette, only consulted when state == Normal.
    int instrumentIndex = -1;
    // Whether this note's track has locked in - a renderer-chosen
    // supplementary cue (e.g. a glow outline), independent of state/color.
    bool lockedIn = false;
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

    // Rails/receptors' base color, as an instrument index (see
    // SceneNote::instrumentIndex) - -1 means no current/preview instrument
    // (Idle or Complete), which a renderer should show as a neutral color.
    int primaryInstrumentIndex = -1;

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
};
