#pragma once

#include <windows.h>
#include <xaudio2.h>

#include <string>
#include <vector>

// Wraps XAudio2: loads WAV stems (each a full instrument loop) and
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
    // stem handle, or -1 on failure (missing file or unsupported format).
    int LoadStem(const std::wstring& wavFilePath);

    // Starts a loaded stem looping seamlessly. Plays once from
    // phaseSeconds to the end of the buffer, then loops the whole buffer
    // forever after - so a loop starting mid-song enters exactly in time
    // rather than restarting from the beginning out of phase.
    void StartLooping(int stemHandle, double phaseSeconds = 0.0);

    // Stops a single stem.
    void Stop(int stemHandle);

    // Stops every currently loaded stem.
    void StopAll();

    // Returns how many seconds of audio have played for a stem, for clock resync.
    double GetPositionSeconds(int stemHandle) const;

    // Returns a stem's total duration in seconds, measured from its actual
    // loaded audio data (not any chart-declared value) - the ground truth
    // for "one complete loop" of that stem.
    double GetStemDurationSeconds(int stemHandle) const;

private:
    struct Stem
    {
        IXAudio2SourceVoice* voice = nullptr;
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        UINT64 loopStartSampleBaseline = 0;
    };

    // Returns a stem's total length in sample frames, derived from its PCM data size.
    static UINT32 GetTotalFrames(const Stem& stem);

    IXAudio2* m_xaudio2 = nullptr;
    IXAudio2MasteringVoice* m_masteringVoice = nullptr;
    std::vector<Stem> m_stems;
    bool m_comInitialized = false;
};
