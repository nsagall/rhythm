#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AudioEngine.h"
#include "BlockSchedule.h"
#include "EditorDocument.h"

// Owns stems and the current BlockSchedule::Schedule; drives AudioEngine to
// match whatever BlockSchedule::Seek says should be audible at the current
// playback position. Replaces the old "audition" PreviewPlayer entirely -
// unlike that simplified model (Learn treated like Break), this reproduces
// exactly what a perfect player would experience, via the analytical
// scheduler (BlockSchedule.h) rather than a simulated live GameSession.
class BlockPlayer
{
public:
    explicit BlockPlayer(AudioEngine& audioEngine);

    // Loads every clip's .wav (that has one) and records its real stem
    // duration. Only valid while stopped. Note: AudioEngine has no
    // "unload" operation, so calling this repeatedly across a long editing
    // session accumulates loaded PCM data rather than freeing the previous
    // set - an accepted, session-scoped limitation (matches the old
    // PreviewPlayer's own documented tradeoff). Clears and reloads
    // everything unconditionally, which is what a wav re-import needs
    // (AudioEngine has no way to refresh an already-loaded stem's content
    // in place) - for the common case of just picking up any clip(s)
    // that don't have a stem yet, see the cheaper EnsureStemsLoaded, which
    // RebuildSchedule already calls automatically.
    bool PrepareStems(const EditorDocument& doc, std::wstring& outError);

    // Re-resolves doc via EditorChartIO::ValidateDocument (getting a real
    // ChartSong), applies ChartClip::ExpandLaneNotesToFillClip per clip
    // using the stem durations PrepareStems/EnsureStemsLoaded already
    // recorded, and rebuilds the schedule via BlockSchedule::Build. Calls
    // EnsureStemsLoaded first, so a clip added since the last full
    // PrepareStems (e.g. via the Clips panel's Detect button, then used by
    // a new block) still gets a real stem duration instead of silently
    // reading as 0 - see EnsureStemsLoaded's own comment for why that
    // matters. On validation failure, leaves the previous schedule in
    // place (so the timeline/playhead don't blink empty over a momentary
    // invalid edit) and returns false with outErrors filled in. Cheap
    // enough to call after every successful debounced revalidation pass.
    bool RebuildSchedule(const EditorDocument& doc, std::vector<std::wstring>& outErrors);

    // Loads a stem for any clip in doc.clips that doesn't already have one
    // (by EditorClip::id) - unlike PrepareStems, this is purely additive:
    // it never clears or reloads a clip that's already loaded, so it's
    // cheap and safe to call on every RebuildSchedule() (including from
    // the debounced live-edit path), regardless of play state. This is
    // what makes a clip added via Detect (or any other live-editing path
    // outside New/Open/SaveAs) actually playable without a full
    // PrepareStems() - previously, RebuildSchedule only ever looked up
    // stems PrepareStems had already recorded, so a brand new clip's
    // duration silently read as 0 until the next Open/SaveAs, which made
    // ClipFitsOneLoop reject it with a misleading "MIDI pattern longer
    // than audio" error and left any block using it unscheduled (and
    // therefore silent) until the document was saved and reloaded. Does
    // NOT handle a wav re-import replacing an already-loaded clip's file
    // content - that still needs a full PrepareStems() (see its own
    // comment on why AudioEngine can't refresh a stem in place).
    bool EnsureStemsLoaded(const EditorDocument& doc, std::wstring& outError);

    const BlockSchedule::Schedule* CurrentSchedule() const;

    // Starts/resumes from the current position (0 if freshly stopped).
    void Play();
    // Stops all audio but remembers the current position, so Play() resumes there.
    void Pause();
    // Stops all audio and resets position to 0.
    void Stop();
    // Jumps the playback position to an arbitrary point - the general
    // "jump to a specific point" entry point used by both the timeline's
    // click/drag-to-seek ruler and SeekToBlockStart below. Clamped to
    // >= 0; not clamped to totalSeconds (Update() is what reconciles an
    // out-of-range position against looping/stopping).
    void SeekToSeconds(double seconds);
    // Convenience: seeks to wherever sectionIndex's own block conceptually
    // begins - sectionIndex is the block's position in
    // doc.blocks/ChartSong::sections, which the schedule's entries
    // preserve 1:1. If sectionIndex has its own BlockSchedule::Entry
    // (Learn/Break), seeks to that entry's sectionStartSeconds. Background/
    // Reset never get an entry of their own (both are zero-duration), so
    // this falls back to wherever the next real entry actually begins -
    // exactly that section's own instant in time - or to the very end if
    // sectionIndex is trailing, past the last real entry.
    void SeekToBlockStart(int sectionIndex);

    void SetLoopWholeSong(bool loop);
    bool LoopWholeSong() const;

    // Advances position by deltaSeconds while Playing, then makes the audio
    // engine match BlockSchedule::Seek(schedule, position) exactly -
    // starting/stopping/re-phasing voices as needed. Since Seek() is a
    // pure function of position, this stays correct after an arbitrary
    // seek, not just forward playback. Call once per frame regardless of
    // state (a no-op unless Playing).
    void Update(double deltaSeconds);

    bool IsPlaying() const;
    bool IsPaused() const;
    double PositionSeconds() const;
    std::wstring NowPlayingText() const;

private:
    void ApplyAudioForPosition();
    // Looks up a stem by the *editor's* stable clip id (m_stems' own key) -
    // only needed while re-resolving in RebuildSchedule, to populate
    // m_stemHandlesByClip (keyed by each clip's own address in
    // m_song.clips instead, which everything else - BlockSchedule::
    // Entry::clip included - uses directly with no further lookup needed).
    StemHandle GetStemForEditorClipId(int editorClipId) const;

    enum class State
    {
        Stopped,
        Paused,
        Playing,
    };

    // One clip's loaded audio, keyed by the editor's stable EditorClip::id
    // (not a position in m_song.clips, which can shift on reorder/delete) -
    // durationSeconds is the stem's real, AudioEngine-measured length, the
    // ground truth BlockSchedule::Build needs, not any chart-declared value.
    struct ClipStem
    {
        int editorClipId = 0;
        StemHandle handle;
        double durationSeconds = 0.0;
    };

    AudioEngine& m_audioEngine;
    std::vector<ClipStem> m_stems; // keyed by EditorClip::id

    // m_song and m_schedule are only ever replaced together, in
    // RebuildSchedule() - see BlockSchedule::Build's own lifetime comment
    // for why that's what makes every ChartClip* below (m_schedule's own,
    // m_stemHandlesByClip's keys, m_activeVoiceClips' entries) safe to hold
    // onto: they all point into m_song.clips, which never changes out from
    // under them except as part of that same rebuild.
    ChartSong m_song;                  // fully resolved (post ExpandLaneNotesToFillClip)
    BlockSchedule::Schedule m_schedule; // built from m_song
    std::unordered_map<const ChartClip*, StemHandle> m_stemHandlesByClip; // keyed by address in m_song.clips

    State m_state = State::Stopped;
    bool m_loopWholeSong = false;
    double m_positionSeconds = 0.0;

    // Which clips are currently audible, so ApplyAudioForPosition only
    // starts/stops a voice when its active state actually changed -
    // unified across Learn/Break/Background, see BlockSchedule::
    // VoiceWindow's own comment for why they all behave the same way once
    // started.
    std::vector<const ChartClip*> m_activeVoiceClips;
};
