#pragma once

#include <windows.h>
#include <xaudio2.h>

#include <string>
#include <vector>

// Wraps XAudio2: loads WAV stems, plays one-shots for tap feedback, and
// starts/stops seamless loops for locked-in instruments. All calls are
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

    // Synthesizes a new stem by placing copies of an already-loaded one-shot
    // stem's audio at each onset (in beats, relative to a repeating span of
    // spanBeats), silence-padded to fill exactly one span at the given
    // tempo. This builds the ambient "whole pattern, looped" buffer for a
    // locked-in instrument from its single hit sample - looping the raw hit
    // alone would just retrigger it every hit-length, not once per span.
    // Requires 16-bit PCM. Returns a new stem handle, or -1 on failure.
    int BuildPatternLoop(int sourceStemHandle, const std::vector<double>& onsetBeats, double spanBeats, double bpm);

    // Plays a loaded stem once, from the start, without looping.
    void PlayOneShot(int stemHandle);

    // Starts a loaded stem looping seamlessly, from the start.
    void StartLooping(int stemHandle);

    // Stops a single stem.
    void Stop(int stemHandle);

    // Stops every currently loaded stem.
    void StopAll();

    // Returns how many seconds of audio have played for a stem, for clock resync.
    double GetPositionSeconds(int stemHandle) const;

private:
    struct Stem
    {
        IXAudio2SourceVoice* voice = nullptr;
        std::vector<BYTE> pcmData;
        WAVEFORMATEX format{};
        UINT64 loopStartSampleBaseline = 0;
    };

    // Submits pcmData as a buffer (optionally looping) and starts playback.
    void SubmitAndPlay(Stem& stem, bool loop);

    IXAudio2* m_xaudio2 = nullptr;
    IXAudio2MasteringVoice* m_masteringVoice = nullptr;
    std::vector<Stem> m_stems;
    bool m_comInitialized = false;
};
