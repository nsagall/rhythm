#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AudioEngine.h"
#include "BlockSchedule.h"
#include "EditorDocument.h"

// Owns stems and the current BlockSchedule::Schedule; drives AudioEngine to match whatever
// BlockSchedule::Seek says should be audible at the current playback position. Reproduces exactly
// what a perfect player would experience, via the analytical scheduler rather than a simulated
// GameSession.
class BlockPlayer
{
public:
    explicit BlockPlayer(AudioEngine& audioEngine);

    // Loads every clip's .wav and records its real stem duration. Only valid while stopped. Clears
    // and reloads everything unconditionally, which is what a wav re-import needs (AudioEngine has
    // no in-place refresh or unload, so this accumulates PCM data across a session). For picking up
    // just newly-added clips, RebuildSchedule already calls the cheaper EnsureStemsLoaded.
    bool PrepareStems(const EditorDocument& doc, std::wstring& outError);

    // Re-resolves doc via EditorChartIO::ValidateDocument, applies ChartClip::ExpandLaneNotesToFillClip
    // per clip using recorded stem durations, and rebuilds the schedule via BlockSchedule::Build.
    // Calls EnsureStemsLoaded first so a clip added since the last PrepareStems still gets a real
    // duration. On validation failure, leaves the previous schedule in place and returns false.
    // Cheap enough to call after every debounced revalidation.
    bool RebuildSchedule(const EditorDocument& doc, std::vector<std::wstring>& outErrors);

    // Loads a stem for any clip in doc.clips that doesn't already have one - purely additive
    // (never clears or reloads), so cheap and safe on every RebuildSchedule(). This is what makes a
    // clip added via Detect playable without a full PrepareStems(); a brand-new clip's duration
    // would otherwise read as 0 and get rejected by ClipFitsOneLoop. Does NOT handle a wav
    // re-import replacing loaded content - that still needs PrepareStems().
    bool EnsureStemsLoaded(const EditorDocument& doc, std::wstring& outError);

    const BlockSchedule::Schedule* CurrentSchedule() const;

    // Starts/resumes from the current position (0 if freshly stopped).
    void Play();
    // Stops all audio but remembers the current position, so Play() resumes there.
    void Pause();
    // Stops all audio and resets position to 0.
    void Stop();
    // Jumps the playback position to seconds - the general seek entry point, used by the timeline
    // ruler and SeekToBlockStart. Clamped to >= 0; not clamped to totalSeconds (Update() reconciles
    // an out-of-range position against looping/stopping).
    void SeekToSeconds(double seconds);
    // Seeks to where sectionIndex's block conceptually begins. sectionIndex is its position in
    // doc.blocks/ChartSong::sections. If it has its own BlockSchedule::Entry (Learn/Break), seeks
    // to that entry's sectionStartSeconds; Background/Reset (zero-duration, no entry) fall back to
    // where the next real entry begins, or to the end if trailing.
    void SeekToBlockStart(int sectionIndex);

    void SetLoopWholeSong(bool loop);
    bool LoopWholeSong() const;

    // Advances position by deltaSeconds while Playing, then makes the audio engine match
    // BlockSchedule::Seek(schedule, position) - starting/stopping/re-phasing voices as needed.
    // Since Seek() is pure, this stays correct after an arbitrary seek. Call once per frame (a
    // no-op unless Playing).
    void Update(double deltaSeconds);

    bool IsPlaying() const;
    bool IsPaused() const;
    double PositionSeconds() const;
    std::wstring NowPlayingText() const;

private:
    void ApplyAudioForPosition();
    // Looks up a stem by the editor's stable clip id (m_stems' key) - only needed in
    // RebuildSchedule to populate m_stemHandlesByClip, which everything else keys by ChartClip*.
    StemHandle GetStemForEditorClipId(int editorClipId) const;

    enum class State
    {
        Stopped,
        Paused,
        Playing,
    };

    // One clip's loaded audio, keyed by EditorClip::id. durationSeconds is the stem's real
    // AudioEngine-measured length, the ground truth BlockSchedule::Build needs.
    struct ClipStem
    {
        int editorClipId = 0;
        StemHandle handle;
        double durationSeconds = 0.0;
    };

    AudioEngine& m_audioEngine;
    std::vector<ClipStem> m_stems; // keyed by EditorClip::id

    // m_song and m_schedule are only ever replaced together, in RebuildSchedule() - which is what
    // makes every ChartClip* below (into m_song.clips) safe to hold onto (see BlockSchedule::Build's
    // lifetime contract).
    ChartSong m_song;                  // Fully resolved (post ExpandLaneNotesToFillClip).
    BlockSchedule::Schedule m_schedule; // Built from m_song.
    std::unordered_map<const ChartClip*, StemHandle> m_stemHandlesByClip; // Keyed by address in m_song.clips.

    State m_state = State::Stopped;
    bool m_loopWholeSong = false;
    double m_positionSeconds = 0.0;

    // Which clips are currently audible, so ApplyAudioForPosition only starts/stops a voice when
    // its active state actually changed.
    std::vector<const ChartClip*> m_activeVoiceClips;
};
