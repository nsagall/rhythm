#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AudioEngine.h"    // StemHandle is stored by value in ClipStem and m_stemHandlesByClip.
#include "BlockSchedule.h"  // BlockSchedule::Schedule m_schedule is a member.
#include "ChartSong.h"      // ChartSong m_song is a member.

struct EditorDocument;

// Owns stems and the current BlockSchedule::Schedule; drives AudioEngine to match whatever
// BlockSchedule::Seek says should be audible at the current playback position. Reproduces what a
// perfect player would experience, via the analytical scheduler.
class BlockPlayer
{
public:
    explicit BlockPlayer(AudioEngine& audioEngine);

    // Loads every clip's .wav and records its real stem duration. Only valid while stopped. Clears
    // and reloads everything unconditionally (AudioEngine has no in-place refresh or unload, so
    // this accumulates PCM data across a session). To pick up just newly-added clips, use
    // EnsureStemsLoaded, which RebuildSchedule calls automatically.
    bool PrepareStems(const EditorDocument& doc, std::wstring& outError);

    // Re-resolves doc via EditorChartIO::ValidateDocument, applies ChartClip::ExpandLaneNotesToFillClip
    // per clip using recorded stem durations, and rebuilds the schedule via BlockSchedule::Build.
    // Calls EnsureStemsLoaded first. On validation failure, leaves the previous schedule in place
    // and returns false.
    bool RebuildSchedule(const EditorDocument& doc, std::vector<std::wstring>& outErrors);

    // Loads a stem for any clip in doc.clips that doesn't already have one - additive only (never
    // clears or reloads). Does NOT handle a wav re-import replacing loaded content; that needs
    // PrepareStems().
    bool EnsureStemsLoaded(const EditorDocument& doc, std::wstring& outError);

    const BlockSchedule::Schedule* CurrentSchedule() const;

    // Starts/resumes from the current position (0 if freshly stopped).
    void Play();

    // Stops all audio but remembers the current position, for Play() to resume from.
    void Pause();

    // Stops all audio and resets position to 0.
    void Stop();

    // Jumps the playback position to seconds. Clamped to >= 0; not clamped to totalSeconds (Update()
    // reconciles an out-of-range position against looping/stopping).
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

    // Looks up a stem by the editor's stable clip id (m_stems' key).
    StemHandle GetStemForEditorClipId(int editorClipId) const;

    enum class State
    {
        Stopped,
        Paused,
        Playing,
    };

    // One clip's loaded audio, keyed by EditorClip::id. durationSeconds is the stem's real
    // AudioEngine-measured length.
    struct ClipStem
    {
        int editorClipId = 0;
        StemHandle handle;
        double durationSeconds = 0.0;
    };

    AudioEngine& m_audioEngine;

    // Keyed by EditorClip::id.
    std::vector<ClipStem> m_stems;

    // m_song, m_schedule and m_stemHandlesByClip are replaced together in RebuildSchedule() - so
    // the ChartClip* into m_song.clips stay valid (see BlockSchedule::Build's lifetime contract).
    // m_song is fully resolved (post ExpandLaneNotesToFillClip); m_stemHandlesByClip is keyed by
    // address into m_song.clips.
    ChartSong m_song;
    BlockSchedule::Schedule m_schedule;
    std::unordered_map<const ChartClip*, StemHandle> m_stemHandlesByClip;

    State m_state = State::Stopped;
    bool m_loopWholeSong = false;
    double m_positionSeconds = 0.0;

    // Which clips are currently audible.
    std::vector<const ChartClip*> m_activeVoiceClips;
};
