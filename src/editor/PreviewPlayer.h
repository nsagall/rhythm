#pragma once

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "EditorDocument.h"

// A lightweight "audition" sequencer for hearing an EditorDocument's
// section sequence play out, without any judging/input - deliberately NOT
// a reuse of GameSession (that's the full judged-player experience, out of
// scope for the editor's preview). Modeled on GameSession::BeginSection's
// per-kind dispatch, but driven purely by AudioEngine::IsPlaying() polling
// since there's no SongClock-anchored input to stay in sync with:
//   - Learn is treated exactly like Break (there's no player to judge
//     against): stop everything else, loop the clip loop_count times,
//     advance once it naturally finishes.
//   - Break: same as Learn, above.
//   - Reset: stop everything, advance immediately (no waiting).
//   - Background: queue the clip (don't touch playback yet), advance
//     immediately; the queued clip actually starts, looping forever, the
//     moment the section after it begins - mirroring chart_rework.txt's
//     "starts when we get to the next break or section block".
class PreviewPlayer
{
public:
    explicit PreviewPlayer(AudioEngine& audioEngine);

    // Loads every clip's .wav (that has one) into the audio engine ready
    // for playback. Only valid while stopped - swapping stems out from
    // under a playing voice isn't supported, and isn't needed since the
    // editor only calls this between play-throughs. Note: AudioEngine has
    // no "unload" operation, so calling this repeatedly across a long
    // editing session (each time the clip set changes) accumulates loaded
    // PCM data rather than freeing the previous set - an accepted,
    // session-scoped limitation, not something a user would notice short
    // of very heavy re-importing.
    bool PrepareForDocument(const EditorDocument& doc, std::wstring& outError);

    // Starts playback: from the beginning if stopped, or by restarting the
    // current section from its own top if resuming from Paused (this is an
    // audition tool, not a sample-accurate scrubber - Pause/Play doesn't
    // preserve mid-section playback position).
    void Play();

    // Stops all audio but remembers the current section, so Play() resumes there.
    void Pause();

    // Stops all audio and resets to the very beginning.
    void Stop();

    // Jumps directly to the start of sectionIndex. No-op while stopped
    // (nothing to seek within yet) - callers should gate on IsPlaying()/
    // IsPaused() before offering a seek control.
    void SeekToSection(int sectionIndex);

    void SetLoopWholeSong(bool loop);
    bool LoopWholeSong() const;

    // Advances section state; call once per frame regardless of state (a
    // no-op unless actively Playing and waiting on a Learn/Break section's
    // stem to finish its loop_count passes).
    void Update();

    bool IsPlaying() const;
    bool IsPaused() const;
    int CurrentSectionIndex() const;         // -1 if stopped
    int CurrentLoopNumber() const;           // 1-based; 0 if nothing is currently looping
    int TotalLoopsForCurrentSection() const; // 0 if nothing is currently looping
    std::wstring NowPlayingText() const;

private:
    void BeginSection(int sectionIndex);
    StemHandle GetStemForClip(int clipId) const;

    enum class State
    {
        Stopped,
        Paused,
        Playing,
    };

    struct ClipStem
    {
        int clipId = 0;
        StemHandle handle;
    };

    AudioEngine& m_audioEngine;
    const EditorDocument* m_doc = nullptr;
    std::vector<ClipStem> m_stems;

    State m_state = State::Stopped;
    bool m_loopWholeSong = false;

    int m_currentSectionIndex = -1;
    int m_currentClipId = -1;
    int m_currentSectionLoopCount = 1;
    // Valid only while actively waiting on a Learn/Break section's stem to
    // finish its loop_count passes; invalid whenever there's nothing to
    // poll (Reset/Background resolve inline in BeginSection and never
    // leave the player waiting on them).
    StemHandle m_waitingStem;

    // Set by a Background section, consumed by the very next BeginSection
    // call (which is exactly "the next section begins").
    int m_queuedBackgroundClipId = -1;
    std::vector<int> m_activeBackgroundClipIds;
};
