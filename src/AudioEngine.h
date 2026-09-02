#pragma once

#include <windows.h>
#include <xaudio2.h>

#include <string>
#include <vector>

// An opaque handle to a loaded stem - a distinct type from a plain int (and from SfxHandle).
struct StemHandle
{
    int value = -1;

    bool IsValid() const
    {
        return value >= 0;
    }
};

inline bool operator==(StemHandle a, StemHandle b)
{
    return a.value == b.value;
}

inline bool operator!=(StemHandle a, StemHandle b)
{
    return !(a == b);
}

// An opaque handle to a loaded one-shot sound effect (LoadSfx/PlaySfx) - a distinct type from StemHandle.
struct SfxHandle
{
    int value = -1;

    bool IsValid() const
    {
        return value >= 0;
    }
};

// Wraps XAudio2: loads WAV stems (each one clip's full loop) and starts/stops them, seeking to the
// correct phase so a loop entering mid-song stays in sync with the beat grid. All calls are
// synchronous and UI-thread-safe (XAudio2 runs its own engine thread).
class AudioEngine
{
public:
    ~AudioEngine();

    // Initializes COM and the XAudio2 engine + mastering voice.
    bool Initialize();

    // Stops all voices and tears down the XAudio2 engine.
    void Shutdown();

    // Loads a PCM WAV file and creates a source voice for it. Returns a
    // stem handle, or an invalid one on failure (missing file or unsupported format).
    StemHandle LoadStem(const std::wstring& wavFilePath);

    // Starts a loaded stem looping seamlessly.
    //   stemHandle   - the stem to play.
    //   phaseSeconds - offset into the buffer to start at; it plays once from here to the end, then
    //                  loops the whole buffer, so a loop starting mid-song enters in phase.
    //   volume       - playback volume (1.0 = unity gain).
    //   loopCount    - 0 loops forever until Stop(); a positive value schedules exactly that many
    //                  passes, after which the voice stops itself sample-accurately inside XAudio2.
    void StartLooping(StemHandle stemHandle, double phaseSeconds = 0.0, float volume = 1.0f, int loopCount = 0);

    // Changes the volume of a stem (1.0 = unity gain), without otherwise affecting playback.
    void SetVolume(StemHandle stemHandle, float volume);

    // Returns a stem's current voice volume (1.0 = unity gain), or -1.0 if the handle is invalid.
    float GetVolume(StemHandle stemHandle) const;

    // Stops a single stem.
    void Stop(StemHandle stemHandle);

    // Stops every currently loaded stem.
    void StopAll();

    // Stops every currently loaded stem except keep. Becomes StopAll() if keep is invalid.
    void StopAllExcept(StemHandle keep);

    // Pauses every currently loaded stem in place - unlike Stop()/StopAll(), keeps each voice's
    // queued buffer, so ResumeAll() continues from where it left off. A no-op for a stem with
    // nothing queued.
    void PauseAll();

    // Resumes every stem paused by PauseAll(). A no-op for a stem with nothing queued.
    void ResumeAll();

    // Returns how many seconds of audio have played for a stem, for clock resync.
    double GetPositionSeconds(StemHandle stemHandle) const;

    // Returns whether a stem's voice is still actively producing audio. False for an invalid
    // handle, or once a finite loop_count's last pass has finished and the voice stopped itself
    // (GetPositionSeconds() has then frozen rather than kept advancing).
    bool IsPlaying(StemHandle stemHandle) const;

    // Returns a stem's total duration in seconds, measured from its loaded audio data (not any
    // chart-declared value) - the ground truth for "one complete loop".
    double GetStemDurationSeconds(StemHandle stemHandle) const;

    // Loads a short PCM WAV file for one-shot, fire-and-forget playback (UI cues, not chart musical
    // content). Returns an SfxHandle, or an invalid one on failure - same contract as LoadStem.
    SfxHandle LoadSfx(const std::wstring& wavFilePath);

    // Plays sfx once from the start at unity gain - a no-op for an invalid handle. Safe to call
    // again before a previous play finishes: each SfxHandle round-robins a small voice pool
    // (c_SfxVoicePoolSize) so a rapid re-fire doesn't cut the previous one off.
    void PlaySfx(SfxHandle sfx);

private:
    // One loaded WAV's playback state: the source voice, the raw PCM data XAudio2 streams from
    // (kept alive for the voice's lifetime - XAudio2 doesn't copy it), its format, and the
    // SamplesPlayed count at the current loop's start.
    struct Stem
    {
        IXAudio2SourceVoice* voice = nullptr;
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        UINT64 loopStartSampleBaseline = 0;

        std::wstring wavFilePath; // Used by StartLooping's same-file-twice assertion.
    };

    // Returns a stem's total length in sample frames, derived from its PCM data size.
    static UINT32 GetTotalFrames(const Stem& stem);

    // Asserts if any OTHER loaded stem sharing playingHandle's wavFilePath is currently audible.
    void AssertNoOtherStemForSameFilePlaying(StemHandle playingHandle) const;

    // Ramps every voice in stems down to silence over ~10ms, then Stop()s/flushes them - shared by
    // Stop/StopAll/StopAllExcept, so several voices stopping at once don't click. Blocks the calling
    // thread for the fade.
    void FadeOutAndStop(const std::vector<Stem*>& stems);

    // How many source voices each loaded Sfx gets, round-robinned by PlaySfx.
    static constexpr int c_SfxVoicePoolSize = 3;

    // One loaded one-shot sound's playback state - like Stem but with a small fixed voice pool.
    struct Sfx
    {
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        IXAudio2SourceVoice* voices[c_SfxVoicePoolSize] = {};
        int nextVoice = 0;
    };

    IXAudio2* m_xaudio2 = nullptr;
    IXAudio2MasteringVoice* m_masteringVoice = nullptr;
    std::vector<Stem> m_stems;
    std::vector<Sfx> m_sfx;
    bool m_comInitialized = false;
};
