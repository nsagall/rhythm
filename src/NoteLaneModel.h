#pragma once

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
    // One note visible on screen for a single lane: where it starts and
    // how long it lasts, both in beats - NotesInRange's own result type,
    // before CollectNotes resolves it into a fully-fledged SceneNote.
    struct BeatRange
    {
        double startBeat = 0.0;
        double durationBeats = 0.0;
    };

    // Returns every note (from one lane's repeating pattern) whose span
    // overlaps [fromBeat, toBeat] at all, not just ones whose start falls
    // inside it - a note that started earlier but whose tail carries past
    // fromBeat must stay visible until it actually scrolls off. originBeat
    // is this clip's own persistent phase origin (GameSession::
    // CurrentClipOriginBeat/PreviewClipOriginBeat): the pattern repeats
    // every spanBeats starting from there, not from absolute beat 0,
    // matching how GameSession itself judges these notes (ChartTiming::
    // NextOnsetAfter/FreshOnsetForAllLanes).
    static std::vector<BeatRange> NotesInRange(double originBeat, double fromBeat, double toBeat,
                                                const std::vector<LaneNote>& notes, double spanBeats);

    // Returns clip's position in ChartSong::clips, or -1 for nullptr -
    // stable for the whole session, since m_song is only ever reassigned
    // wholesale by LoadChart, never mutated element-by-element in place. A
    // renderer resolves this to an actual color; this class never does.
    static int ClipIndex(const GameSession& session, const ChartClip* clip);

    // Appends drawClip's notes into outNotes, resolving each one's
    // NoteVisualState from session's judging when judged is true (held/
    // hit/miss/normal), or leaving every note Normal for an unjudged
    // preview pass. fromBeat is a shared per-lane start point, except a
    // negative sentinel means "each lane's own first required note"
    // (PreviewFirstOnsetBeatForLane) - used for a not-yet-started clip, so
    // its lanes reveal one at a time instead of all at once.
    void CollectNotes(const GameSession& session, const ChartClip* drawClip, double originBeat, bool judged,
                       double fromBeat, double upperBoundBeat, std::vector<SceneNote>& outNotes) const;

    bool m_prevLockedIn = false;
    bool m_prevNotesHandoff = false;
};
