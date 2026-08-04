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

// One clip's identity and currently-relevant judged state, for rendering
// purposes only - NOT the same object as GameSession's own private
// per-clip playback voice (audio/timing internals a renderer has no
// business touching). Lives in NoteLaneScene::clipInstances, one per clip
// in the song, rebuilt fresh every frame; every SceneNote below points
// into that same array rather than carrying its own copy of this state,
// so every note belonging to the same clip in the same frame necessarily
// agrees about it.
struct ClipInstance
{
    // This clip's stable position in ChartSong::clips, or -1 for the
    // "no clip" placeholder a null SceneNote::clip/NoteLaneScene::
    // primaryClip stands in for - a renderer resolves this to an actual
    // color via its own palette.
    int index = -1;

    // Whether the section currently judging this clip has locked in - a
    // renderer-chosen supplementary cue (e.g. a glow outline), independent
    // of any one note's own state/color. Only ever true for whichever
    // clip is being live-judged right now - every other clip's instance
    // reads false, even one locked in earlier by a since-ended section.
    bool lockedIn = false;
};

// What a single note looks like once NoteLaneModel has resolved the
// GameSession's judging for it.
enum class NoteVisualState
{
    Normal, // not yet interacted with - a renderer colors this by clip->index
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
    // notes/explodingNotes - points into that same frame's
    // NoteLaneScene::clipInstances, so it's only ever valid for as long as
    // that NoteLaneScene is (see clipInstances' own comment for why that's
    // safe despite the vector living in the very struct being returned by
    // value).
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

    // One entry per clip in the song, index-for-index with ChartSong::
    // clips - every SceneNote::clip (in notes/explodingNotes) and
    // primaryClip below point somewhere into this array. NoteLaneModel
    // sizes and fully populates this once, before taking any pointer into
    // it, and never resizes it again for the rest of that BuildScene call
    // - a std::vector's element addresses stay stable across everything
    // except a resize, and this one never gets one after that point, so
    // those pointers stay valid for exactly as long as this NoteLaneScene
    // does (including surviving the move out of BuildScene's return).
    std::vector<ClipInstance> clipInstances;

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
};
