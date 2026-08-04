#pragma once

#include <memory>
#include <vector>

#include "GameSession.h"
#include "NoteLaneScene.h"

// Turns one GameSession's current judging/lock-in state into a
// NoteLaneScene - the only part of the note lane that knows anything about
// game rules (judging, lock-in, section/clip identity, phase timing).
// Never touches HDC/GDI or a pixel position, and owns no continuous
// animation state (confetti/explosion/ripples/flash - see
// NoteLaneRenderer.h for that), so a renderer can be swapped out freely
// without this class changing, and this class's output can be inspected
// headlessly.
class NoteLaneModel
{
public:
    // Call once per frame; internally tracks lock-in/handoff edges across
    // calls (see NoteLaneScene::justLockedIn/justHandedOff), so calls must
    // be sequential real frames of the same session, not arbitrary replays.
    NoteLaneScene BuildScene(const GameSession& session);

private:
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

    // Creates a fresh instance for chartClip (identity, freshly-resolved
    // color, lockedIn=false), or returns null if chartClip is null. The
    // color lookup needs chartClip's position in session.Song().clips -
    // computed here, once, purely to pick a palette entry (ClipColor::
    // ForIndex) for this brand new instance; never stored or reused as a
    // lookup key afterward, unlike the index pattern this replaced.
    static std::unique_ptr<ClipInstance> MakeClipInstance(const GameSession& session, const ChartClip* chartClip);

    // Keeps m_previousClip/m_currentClip/m_nextClip in sync with
    // session.CurrentClip()/PreviewClip() for this frame - called once, at
    // the top of BuildScene. See the members' own comments for what each
    // slot means and when it changes.
    void UpdateClipInstances(const GameSession& session);

    // Persistent (NOT rebuilt every frame) identity for the up-to-three
    // clip playthroughs the note lane ever cares about. A fresh
    // ClipInstance is only created when a new playthrough actually begins
    // (m_currentClip's own chartClip differs from session.CurrentClip()),
    // reusing m_nextClip directly (same object, no rebuild) when it
    // already represents the clip that's now going live - a preview that
    // correctly predicted what's next becomes that same instance, not an
    // equivalent new one.
    //
    // The one being actively judged/shown right now - mirrors
    // session.CurrentClip()'s own identity exactly (null when there isn't
    // one, e.g. Idle/CountIn). lockedIn is refreshed from session every
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
    // The preview clip - mirrors session.PreviewClip()'s own identity.
    std::unique_ptr<ClipInstance> m_nextClip;

    bool m_prevLockedIn = false;
    bool m_prevNotesHandoff = false;
};
