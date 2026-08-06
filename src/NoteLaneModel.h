#pragma once

#include <memory>
#include <vector>

#include "GameSession.h"
#include "NoteLaneScene.h"

// Turns one GameSession's current judging/passing state into a
// NoteLaneScene - the only part of the note lane that knows anything about
// game rules (judging, passing, section/clip identity, phase timing).
// Never touches HDC/GDI or a pixel position, and owns no continuous
// animation state (confetti/explosion/ripples/flash - see
// NoteLaneRenderer.h for that), so a renderer can be swapped out freely
// without this class changing, and this class's output can be inspected
// headlessly.
class NoteLaneModel
{
public:
    // Call once per frame; internally tracks passing/handoff edges across
    // calls (see NoteLaneScene::justLockedIn/justFailed/justHandedOff), so
    // calls must be sequential real frames of the same session, not
    // arbitrary replays. Also tracks last frame's beat position (see
    // m_prevNowBeat) to detect exactly which onsets of an already-exploded
    // Pass-mode clip crossed the judge line this frame (see
    // NoteLaneScene::passLineHitLanes) - same sequential-real-frames
    // requirement.
    NoteLaneScene BuildScene(const GameSession& session);

private:
    // Detects a chart switch (session.Song().clips' own buffer address
    // changed since last call - see this class's own .cpp for why that's
    // the right signal) and, if so, clears m_previousClip/m_currentClip/
    // m_nextClip before anything else runs this frame - every
    // ClipInstance::chartClip they hold points into the *previous* song's
    // now-destroyed ChartClip vector otherwise. Called first thing in
    // BuildScene, ahead of UpdateClipInstances and anything that might
    // dereference a stale instance's chartClip.
    void ResetIfSongChanged(const GameSession& session);

    // Returns every note (from one lane's repeating pattern) whose span
    // overlaps [fromBeat, toBeat] at all, not just ones whose start falls
    // inside it - a note that started earlier but whose tail carries past
    // fromBeat must stay visible until it actually scrolls off. originBeat
    // is this clip's own persistent phase origin (GameSession::
    // CurrentClipOriginBeat/PreviewClipOriginBeat): the pattern repeats
    // every spanBeats starting from there, not from absolute beat 0,
    // matching how GameSession itself judges these notes (ChartTiming::
    // NextOnsetAfter/FreshOnsetForAllLanes). Each returned SceneNote
    // carries lane/startBeat/durationBeats only - state stays Normal and
    // clip stays null, since pure beat-tiling arithmetic has no way to
    // know either; the caller fills both in.
    static std::vector<SceneNote> NotesInRange(int lane, double originBeat, double fromBeat, double toBeat,
                                                const std::vector<LaneNote>& notes, double spanBeats);

    // Appends instance's clip's notes into scene.notes, resolving each
    // one's NoteVisualState from session's judging when judged is true
    // (held/hit/miss/normal), or leaving every note Normal for an unjudged
    // preview pass. fromBeat is a shared per-lane start point, except a
    // negative sentinel means "each lane's own first required note"
    // (PreviewFirstOnsetBeatForLane) - used for a not-yet-started clip, so
    // its lanes reveal one at a time instead of all at once. No-op if
    // instance is null (nothing to draw).
    void CollectNotes(const GameSession& session, const ClipInstance* instance, double originBeat, bool judged,
                       double fromBeat, double upperBoundBeat, NoteLaneScene& scene) const;

    // Creates a fresh instance identified by (chartClip, startBeat) -
    // freshly-resolved color, passing=false - or returns null if chartClip
    // is null. startBeat is the caller's responsibility: for a clip just
    // becoming current/preview for real, that's session.
    // CurrentClipOriginBeat()/PreviewClipOriginBeat(); for a predicted loop
    // repeat, it's the current instance's own startBeat plus one spanBeats
    // - see UpdateClipInstances. The color lookup needs chartClip's
    // position in session.Song().clips - computed here, once, purely to
    // pick a palette entry (ClipColor::ForIndex) for this brand new
    // instance; never stored or reused as a lookup key afterward, unlike
    // the index pattern this replaced.
    static std::unique_ptr<ClipInstance> MakeClipInstance(const GameSession& session, const ChartClip* chartClip,
                                                            double startBeat);

    // Keeps m_previousClip/m_currentClip/m_nextClip in sync for this frame
    // - called once, at the top of BuildScene, with that frame's own
    // nowBeat (scene.nowBeat - passed explicitly rather than re-derived,
    // since it's already 0 vs. a real clock read depending on
    // clockRunning). See the members' own comments for what each slot
    // means and when it changes.
    void UpdateClipInstances(const GameSession& session, double nowBeat);

    // Persistent (NOT rebuilt every frame) identity for the up-to-three
    // clip playthroughs the note lane ever cares about. A fresh
    // ClipInstance is only created when a new playthrough actually begins
    // - detected by comparing (chartClip, startBeat) as a pair, not just
    // chartClip, since a clip looping in place keeps the same chartClip but
    // starts a new playthrough (see UpdateClipInstances) - reusing
    // m_nextClip directly (same object, no rebuild) when it already
    // predicted exactly this playthrough - a preview (of either a real next
    // section or a predicted loop repeat) that turned out right becomes
    // that same instance, not an equivalent new one.
    //
    // The one being actively judged/shown right now - mirrors
    // session.CurrentClip()'s own identity exactly (null when there isn't
    // one, e.g. Idle/CountIn). passing is refreshed from session every
    // frame while non-null.
    std::unique_ptr<ClipInstance> m_currentClip;
    // Whatever m_currentClip held immediately before its last transition -
    // exists so a renderer reacting to a handoff/lock-in event has a
    // stable place to find the outgoing clip's identity even after
    // m_currentClip itself has moved on. Not currently read by BuildScene's
    // own explosion-notes path (see its comment for why m_currentClip
    // itself is still correct there), but kept in sync regardless, as the
    // general "what was current a moment ago" answer.
    std::unique_ptr<ClipInstance> m_previousClip;
    // Whatever playthrough is predicted to become current next. Mirrors
    // session.PreviewClip()'s own identity whenever that's non-null (a real
    // section-level preview); otherwise, while the current section is an
    // unlocked Learn section (the case session.PreviewClip() itself has
    // nothing to offer - see its own comment on why), predicts that same
    // clip's own next loop repeat instead, so the note lane always has
    // something legitimate to preview rather than going blank right up
    // until a section transition or lock-in. See UpdateClipInstances.
    std::unique_ptr<ClipInstance> m_nextClip;

    // Last frame's IsPassing() value, purely to edge-detect justLockedIn/
    // justFailed (see NoteLaneScene) - not the same as any ClipInstance::
    // passing field, which is a per-instance snapshot rather than a
    // frame-to-frame edge tracker.
    bool m_prevPassing = false;
    bool m_prevNotesHandoff = false;

    // Last frame's scene.nowBeat, purely so a Pass-mode section that's
    // already locked in (and so had its notes explode immediately - see
    // BuildScene's nextClipShowing override) can detect exactly which of
    // its own onsets crossed the judge line since last frame, to populate
    // NoteLaneScene::passLineHitLanes - a plain snapshot, not an edge flag
    // like m_prevPassing/m_prevNotesHandoff above.
    double m_prevNowBeat = 0.0;

    // The last session.Song().clips.data() ResetIfSongChanged saw - see
    // its own comment for why this is the right chart-switch signal.
    const ChartClip* m_lastSongClipsBase = nullptr;
};
