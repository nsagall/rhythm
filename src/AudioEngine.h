#pragma once

#include <windows.h>
#include <xaudio2.h>

#include <string>
#include <vector>

// A distinct type for referring to a loaded stem, so it can't be silently
// mixed up at compile time with other unrelated integers (e.g. a clip
// index into a chart, which is a very easy accidental swap since both are
// small ints that are often numerically equal).
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

// A distinct type for a loaded one-shot sound effect (LoadSfx/PlaySfx) -
// same "can't be silently confused with an unrelated int" reasoning as
// StemHandle, and deliberately a different type from it: an SfxHandle is
// never valid to pass to any stem-looping call (StartLooping, SetVolume,
// ...), nor a StemHandle to PlaySfx.
struct SfxHandle
{
    int value = -1;

    bool IsValid() const
    {
        return value >= 0;
    }
};

// Wraps XAudio2: loads WAV stems (each one clip's full loop) and
// starts/stops them playing, seeking to the correct phase so a loop
// entering mid-song stays in sync with the beat grid. All calls are
// synchronous and safe to make from the UI thread - XAudio2 runs its own
// internal engine thread, so there is no background-thread machinery here.
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

    // Starts a loaded stem looping seamlessly, at the given volume (1.0 =
    // unity gain). Plays once from phaseSeconds to the end of the buffer,
    // then loops the whole buffer after - so a loop starting mid-song
    // enters exactly in time rather than restarting from the beginning out
    // of phase. loopCount == 0 (default) loops forever until Stop() is
    // called - for a clip whose total duration isn't known when it starts
    // (a learn or background clip, whose eventual stop time depends on
    // future player input). loopCount > 0 instead schedules exactly that
    // many total passes through the loop, after which the voice stops on
    // its own, sample-accurately, entirely inside XAudio2 - no explicit
    // Stop() call needed, and no risk of a fragment of the loop's
    // beginning bleeding through before a polling-driven Stop() call
    // catches up to the intended stop instant. Use this for a clip whose
    // total loop count is already known the moment it starts (a solo
    // clip's loop_count).
    void StartLooping(StemHandle stemHandle, double phaseSeconds = 0.0, float volume = 1.0f, int loopCount = 0);

    // Changes the volume of an already-playing (or not-yet-playing) stem
    // without otherwise affecting playback (1.0 = unity gain).
    void SetVolume(StemHandle stemHandle, float volume);

    // Returns a stem's current voice volume (1.0 = unity gain), or -1.0 if the handle is invalid.
    float GetVolume(StemHandle stemHandle) const;

    // Stops a single stem.
    void Stop(StemHandle stemHandle);

    // Stops every currently loaded stem.
    void StopAll();

    // Stops every currently loaded stem except keep. For a caller about to
    // immediately restart keep's own voice anyway (see GameSession::
    // BeginSection's Break case, which starts its own section's clip right
    // after silencing everything else - if that clip happens to be one
    // that was already playing uninterrupted, this is exactly that case) -
    // stopping keep here too, only to have StartLooping's own Stop()/
    // FlushSourceBuffers() immediately stop-and-reflush it again moments
    // later, is pure redundant overhead that only widens the window during
    // which a stray fragment of stale audio could bleed through: Stop()/
    // FlushSourceBuffers() take effect at the start of XAudio2's next
    // internal processing quantum, not synchronously, so calling them twice
    // in a row on the same voice doesn't make it stop any faster - it just
    // means there are two chances instead of one for a few milliseconds of
    // in-flight audio to slip out before either takes hold. A no-op for
    // keep if it's invalid (nothing to exclude, so this becomes StopAll()).
    void StopAllExcept(StemHandle keep);

    // Pauses every currently loaded stem in place - unlike Stop()/StopAll(),
    // doesn't flush each voice's queued buffer, so its playback position is
    // preserved for ResumeAll() to continue from exactly where it left off,
    // rather than needing to reseek/restart via StartLooping. A no-op for a
    // stem with nothing queued (never started, or already Stop()'d).
    void PauseAll();

    // Resumes every stem paused by PauseAll(), from exactly where it left
    // off. A no-op for a stem with nothing queued.
    void ResumeAll();

    // Returns how many seconds of audio have played for a stem, for clock resync.
    double GetPositionSeconds(StemHandle stemHandle) const;

    // Returns whether a stem's voice is still actively producing audio.
    // False for an invalid handle, or once a finite loop_count's last pass
    // has finished and the voice has stopped itself - even though nothing
    // has explicitly called Stop() on it yet, and GetPositionSeconds() has
    // accordingly frozen rather than kept advancing.
    bool IsPlaying(StemHandle stemHandle) const;

    // Returns a stem's total duration in seconds, measured from its actual
    // loaded audio data (not any chart-declared value) - the ground truth
    // for "one complete loop" of that stem.
    double GetStemDurationSeconds(StemHandle stemHandle) const;

    // Loads a short PCM WAV file for one-shot, fire-and-forget playback
    // (UI cues - e.g. a streak-multiplier-up "happy" sound - not a chart's
    // own musical content, which always goes through LoadStem/StartLooping
    // instead). Returns an SfxHandle, or an invalid one on failure (missing
    // file or unsupported format) - same failure contract as LoadStem.
    SfxHandle LoadSfx(const std::wstring& wavFilePath);

    // Plays sfx once from the start, at unity gain, without looping - a
    // no-op for an invalid handle. Safe to call again before a previous
    // play of the same sfx has finished: each SfxHandle round-robins a
    // small pool of voices (see kSfxVoicePoolSize) so a rapid re-fire gets
    // its own voice instead of cutting the previous one off.
    void PlaySfx(SfxHandle sfx);

private:
    // One loaded WAV's playback state: the source voice actually playing
    // it, the raw PCM data XAudio2 streams from (kept alive for the
    // voice's whole lifetime, since XAudio2 doesn't copy it), its format,
    // and the voice's own SamplesPlayed count at the moment its current
    // loop started, so GetPositionSeconds can measure elapsed-since-loop-
    // start instead of elapsed-since-the-voice-was-first-created.
    struct Stem
    {
        IXAudio2SourceVoice* voice = nullptr;
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        UINT64 loopStartSampleBaseline = 0;

        // The file this stem was loaded from (LoadStem's own argument,
        // kept around) - used only by StartLooping's same-file-twice
        // assertion below, since two DIFFERENT stems can end up wrapping
        // the same .wav (e.g. a chart bug, or a reload leaving an old
        // stem behind) in a way m_stems' own index/StemHandle can't catch.
        std::wstring wavFilePath;
    };

    // Returns a stem's total length in sample frames, derived from its PCM data size.
    static UINT32 GetTotalFrames(const Stem& stem);

    // Asserts if any OTHER loaded stem sharing playingHandle's wavFilePath
    // is currently audible - see StartLooping's own call site comment.
    void AssertNoOtherStemForSameFilePlaying(StemHandle playingHandle) const;

    // How many source voices each loaded Sfx gets, round-robinned by
    // PlaySfx - enough to survive a rapid re-fire (e.g. two multiplier
    // tier-ups a beat apart) without one play cutting its predecessor off,
    // without needing a real voice-availability check.
    static constexpr int kSfxVoicePoolSize = 3;

    // One loaded one-shot sound's playback state - same pcmData/format
    // shape as Stem, but a small fixed pool of voices instead of Stem's
    // single one, since PlaySfx is fire-and-forget rather than
    // start-once-and-hold like a stem's loop.
    struct Sfx
    {
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        IXAudio2SourceVoice* voices[kSfxVoicePoolSize] = {};
        int nextVoice = 0;
    };

    IXAudio2* m_xaudio2 = nullptr;
    IXAudio2MasteringVoice* m_masteringVoice = nullptr;
    std::vector<Stem> m_stems;
    std::vector<Sfx> m_sfx;
    bool m_comInitialized = false;
};
