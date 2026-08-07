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
// passing resets fresh every time and identity/color needs to be
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

    // Whether this playthrough is currently passing, for a renderer's own
    // supplementary "correct" cue (a glow outline) - independent of any one
    // note's own state/color. Reset false whenever a new ClipInstance is
    // created (a fresh playthrough hasn't earned anything yet), kept in
    // sync with the session every frame while this instance is current -
    // except for a DontFail clip, which never sets this true at all (always
    // false for its entire run), even though GameSession::IsPassing()
    // itself does flip back and forth for one same as it does for Pass -
    // DontFail conveys its own progress through the hits meter bar instead
    // (see NoteLaneScene::hitsMeterProgress), not a glow that would
    // otherwise be gaining and losing itself mid-clip on every miss/recovery.
    // In Pass mode this only ever goes false->true once, same as before.
    bool passing = false;
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

    // Only meaningful when state == Hit - mirrors GameSession::OnsetPrecise:
    // false for a correct press that landed outside half the tolerance
    // window (a "partial miss" - still counted as a hit, worth less score,
    // but a renderer should color it differently from a precise one - see
    // NoteLaneGdiRenderer::ColorForNote/kNoteColorHitImprecise).
    bool precise = true;

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
    // tracking its own copy of the session's passing/handoff state.
    bool justLockedIn = false;
    bool justHandedOff = false;
    // DontFail mode only: the current clip was passing and just reverted to
    // failing (see GameSession::IsPassing()) - never true in Pass mode,
    // where passing is a one-way latch. The reverse direction (failing ->
    // passing, including a first-ever pass) is justLockedIn above; it
    // didn't need a name of its own since it already fires correctly for
    // both cases with no assumption baked in that it can only happen once.
    bool justFailed = false;
    // Candidate notes for whichever of the above fired this frame (may be
    // more than one at once, and the triggers' notes are not distinguished
    // from each other since they're drawn identically) - a renderer
    // reacting to these should still apply its own on-screen visibility
    // check first, exactly as it would for NoteLaneScene::notes.
    std::vector<SceneNote> explodingNotes;

    // Lanes where a Pass-mode section's own notes - already exploded away
    // and never drawn again for the rest of its run (see
    // NoteLaneModel::BuildScene's nextClipShowing override) - crossed the
    // judge line this frame anyway, since that clip's audio keeps looping
    // in the background regardless. A renderer should flash a "hit" at the
    // judge line for each of these exactly as it would for a real judged
    // Hit, even though there's no on-screen note to point at. May repeat
    // the same lane more than once if more than one note crossed within a
    // single frame - reacting to each is idempotent (just re-triggers the
    // same flash), so no de-duplication is needed here.
    std::vector<int> passLineHitLanes;

    // Progress for the "hits meter" panel beside the playfield, clamped to
    // [0,1] - only meaningful while showHitsMeter is true, and means two
    // different things depending on hitsMeterIsDontFail:
    //   - Pass mode: streak divided by the clip's own hitsRequired - "how
    //     close to locking in."
    //   - DontFail mode: how far nowBeat has gotten through the clip's
    //     current loop repetition (its own spanBeats) - "how far through
    //     the clip," live while passing, frozen the instant a miss drops
    //     back to failing (so it visibly stops filling rather than
    //     resetting), jumping to the real live value again the instant it
    //     locks back in (real time kept passing while it was frozen), and
    //     reset to 0 if a whole loop repetition elapses while still failing
    //     (that attempt genuinely restarted from the top). See
    //     NoteLaneModel::BuildScene's own comment for the implementation.
    double hitsMeterProgress = 0.0;

    // True whenever the hits meter should be visible. Pass mode: there's a
    // current learn section and it isn't passing right now - false for the
    // entire rest of the section's run once it first locks in (that
    // section's own justLockedIn frame is the moment the meter should pop
    // instead of just vanishing - see INoteLaneRenderer::Draw), since
    // passing is a one-way latch there and there's nothing left to track.
    // DontFail mode: true for the entire learn section, passing or not -
    // its own hitsMeterProgress (above) is meaningful either way, unlike
    // Pass's streak-based number, which has nothing to report once locked
    // in.
    bool showHitsMeter = false;

    // Which of two lock-in treatments the hits meter should get on a
    // justLockedIn frame: true for a DontFail clip (a small celebratory
    // spark burst - see NoteLaneRenderer's AppendHitsMeterExplosion - since
    // the meter stays visible and can lock in many times over one section's
    // run, so this is a "nice job" flourish, not a disappearance), false for
    // a Pass clip (a confetti burst right as the meter itself vanishes for
    // good - passing is a one-way latch there, so this is its one and only
    // lock-in for the whole section). Meaningless (and left at its default)
    // whenever showHitsMeter is false and no lock-in is happening this
    // frame - a renderer only ever needs to read it on a justLockedIn
    // frame, but it's populated any time there's a current learn clip so
    // it's always correct by the time that frame arrives.
    bool hitsMeterIsDontFail = false;

    std::wstring statusText;

    // The running score, already formatted for display (e.g. L"Score 12,340") -
    // see GameSession::CurrentScore(). Drawn by INoteLaneRenderer::DrawHud
    // alongside statusText, right-aligned in the same HUD panel so it's
    // visible for the whole song, not just after it ends.
    std::wstring scoreText;

    // The current section's not-yet-banked score, already formatted (e.g.
    // L"+120") - empty whenever GameSession::PendingScore() is 0, so a
    // renderer can treat "nothing to show" and "text to draw" as the same
    // check. Meant to read as points visibly building up, separate from the
    // permanent total in scoreText - see GameSession::ScoreEvent/
    // ConsumeScoreEvents for the moment those points move (or vanish) as a
    // renderer-owned animation instead.
    std::wstring pendingScoreText;

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
