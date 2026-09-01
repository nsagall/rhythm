#pragma once

#include <memory>
#include <vector>

#include "NoteLaneScene.h"  // ClipPlaythrough is held by value (via unique_ptr) in the members below.

class GameSession;
class ChartClip;
struct LaneNote;

// Turns one GameSession's current judging/passing state into a NoteLaneScene - the only part of the
// note lane that knows game rules (judging, passing, section/clip identity, phase timing). Never
// touches HDC/GDI or a pixel position, and owns no continuous animation state (see
// NoteLaneRenderer.h), so a renderer can be swapped freely and this class's output inspected headlessly.
class NoteLaneModel
{
public:
    // Builds this frame's scene. Call once per real frame of the same session - internally
    // edge-detects passing/handoff transitions across calls, so arbitrary replays break it.
    NoteLaneScene BuildScene(const GameSession& session);

private:
    // Detects a chart switch (session.Song().clips' buffer address changed) and, if so, clears
    // m_previousClip/m_currentClip/m_nextClip before anything else this frame - each
    // ClipPlaythrough::chartClip they hold otherwise points into the previous song's destroyed
    // ChartClip vector. Called first thing in BuildScene.
    void ResetIfSongChanged(const GameSession& session);

    // Returns every note from one lane's repeating pattern whose span overlaps [fromBeat, toBeat] -
    // including one that started earlier but whose tail carries past fromBeat.
    //   lane      - which lane.
    //   originBeat - the current arrangement's shared origin; the pattern repeats every spanBeats from here.
    //   fromBeat  - lower bound of the visible window, in beats.
    //   toBeat    - upper bound of the visible window, in beats.
    //   notes     - one lane's note list.
    //   spanBeats - the pattern's cycle length, in beats.
    // Each returned SceneNote carries lane/startBeat/durationBeats only; state stays Normal and
    // clip stays null for the caller to fill in.
    static std::vector<SceneNote> NotesInRange(int lane, double originBeat, double fromBeat, double toBeat,
                                                const std::vector<LaneNote>& notes, double spanBeats);

    // Appends instance's clip's notes into scene.notes.
    //   session        - the game session.
    //   instance       - the playthrough whose notes to collect; no-op if null.
    //   originBeat     - the clip's arrangement origin, in beats.
    //   judged         - resolve each note's NoteVisualState from judging (held/hit/miss/normal); false leaves them Normal.
    //   fromBeat       - shared per-lane start beat, or a negative sentinel meaning "each lane's own first required note".
    //   upperBoundBeat - upper bound of the visible window, in beats.
    //   scene          - scene to append into.
    void CollectNotes(const GameSession& session, const ClipPlaythrough* instance, double originBeat, bool judged,
                       double fromBeat, double upperBoundBeat, NoteLaneScene& scene) const;

    // Creates a fresh playthrough identified by (chartClip, startBeat), or returns null if chartClip is null.
    //   session   - the game session (used only to find chartClip's index for its palette color).
    //   chartClip - the clip this playthrough is of.
    //   startBeat - the playthrough's start beat; the caller's responsibility (origin for a real
    //               start, or the previous instance's startBeat plus one spanBeats for a predicted loop).
    static std::unique_ptr<ClipPlaythrough> MakeClipInstance(const GameSession& session, const ChartClip* chartClip,
                                                            double startBeat);

    // Keeps m_previousClip/m_currentClip/m_nextClip in sync for this frame. Called once at the top
    // of BuildScene.
    //   nowBeat - this frame's nowBeat (scene.nowBeat), passed explicitly since it's 0 or a real
    //             clock read depending on clockRunning.
    void UpdateClipInstances(const GameSession& session, double nowBeat);

    // Persistent (not rebuilt every frame) identity for the up-to-three clip playthroughs the note
    // lane cares about. A fresh ClipPlaythrough is created only when a new playthrough begins -
    // identified by (chartClip, startBeat) as a pair, since a clip looping in place keeps the same
    // chartClip but starts a new playthrough. A preview that turns out right becomes that same
    // instance, not an equivalent new one.

    // The one being actively judged/shown right now - mirrors session.CurrentClip()'s identity
    // (null in Idle/CountIn). passing is refreshed from session every frame while non-null.
    std::unique_ptr<ClipPlaythrough> m_currentClip;
    // Whatever m_currentClip held immediately before its last transition, so a renderer reacting to
    // a handoff/lock-in has a stable place to find the outgoing clip's identity.
    std::unique_ptr<ClipPlaythrough> m_previousClip;
    // Whatever playthrough is predicted to become current next. Mirrors session.PreviewClip() when
    // that's non-null; otherwise, during an unlocked Learn section, predicts that clip's own next
    // loop repeat so the note lane always has something legitimate to preview.
    std::unique_ptr<ClipPlaythrough> m_nextClip;

    // Last frame's IsPassing() value, to edge-detect justLockedIn/justFailed - distinct from any
    // ClipPlaythrough::passing snapshot.
    bool m_prevPassing = false;
    bool m_prevNotesHandoff = false;

    // The last session.Song().clips.data() ResetIfSongChanged saw.
    const ChartClip* m_lastSongClipsBase = nullptr;

    // DontFail mode only: the hits meter's frozen progress and which loop repetition it belongs to,
    // while the current clip is failing. Only read/written while failing; a fresh section always
    // starts passing, so the first failure overwrites whatever's here.
    double m_dontFailFrozenProgress = 0.0;
    // -1.0 means "no freeze recorded yet" - never a real loop-start beat, so it can't be mistaken for one.
    double m_dontFailFrozenLoopStartBeat = -1.0;
};
