#include "AudioEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{

// Reads a PCM WAV file (plain PCM or WAVE_FORMAT_EXTENSIBLE) into pcmData/format.
// Returns false if the file can't be read or isn't a supported PCM WAV.
bool LoadWavFile(const std::wstring& path, std::vector<BYTE>& outPcmData, WAVEFORMATEX& outFormat)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
    {
        return false;
    }

    char riffId[4];
    UINT32 riffSize = 0;
    char waveId[4];
    file.read(riffId, 4);
    file.read(reinterpret_cast<char*>(&riffSize), 4);
    file.read(waveId, 4);
    if (!file || memcmp(riffId, "RIFF", 4) != 0 || memcmp(waveId, "WAVE", 4) != 0)
    {
        return false;
    }

    WAVEFORMATEX format{};
    std::vector<BYTE> pcmData;
    bool haveFormat = false;
    bool haveData = false;

    while (file && (!haveFormat || !haveData))
    {
        char chunkId[4];
        UINT32 chunkSize = 0;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!file)
        {
            break;
        }

        if (memcmp(chunkId, "fmt ", 4) == 0)
        {
            UINT32 readSize = std::min<UINT32>(chunkSize, sizeof(WAVEFORMATEX));
            file.read(reinterpret_cast<char*>(&format), readSize);
            if (chunkSize > readSize)
            {
                file.seekg(chunkSize - readSize, std::ios::cur);
            }
            haveFormat = true;
        }
        else if (memcmp(chunkId, "data", 4) == 0)
        {
            pcmData.resize(chunkSize);
            file.read(reinterpret_cast<char*>(pcmData.data()), chunkSize);
            haveData = true;
        }
        else
        {
            file.seekg(chunkSize, std::ios::cur);
        }

        if (chunkSize % 2 != 0)
        {
            file.seekg(1, std::ios::cur); // RIFF chunks are word-aligned
        }
    }

    if (!haveFormat || !haveData)
    {
        return false;
    }

    bool isSupportedFormat = format.wFormatTag == WAVE_FORMAT_PCM || format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    if (!isSupportedFormat)
    {
        return false;
    }

    outFormat = format;
    outPcmData = std::move(pcmData);
    return true;
}

} // namespace

// Ensures Shutdown() has run before the engine is destroyed.
AudioEngine::~AudioEngine()
{
    Shutdown();
}

// Initializes COM and the XAudio2 engine + mastering voice.
bool AudioEngine::Initialize()
{
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInitialized = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
    {
        return false;
    }

    if (FAILED(XAudio2Create(&m_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        return false;
    }

    if (FAILED(m_xaudio2->CreateMasteringVoice(&m_masteringVoice)))
    {
        return false;
    }

    return true;
}

// Stops all voices and tears down the XAudio2 engine.
void AudioEngine::Shutdown()
{
    StopAll();

    for (Stem& stem : m_stems)
    {
        if (stem.voice)
        {
            stem.voice->DestroyVoice();
            stem.voice = nullptr;
        }
    }
    m_stems.clear();

    for (Sfx& sfx : m_sfx)
    {
        for (IXAudio2SourceVoice*& voice : sfx.voices)
        {
            if (voice)
            {
                voice->DestroyVoice();
                voice = nullptr;
            }
        }
    }
    m_sfx.clear();

    if (m_masteringVoice)
    {
        m_masteringVoice->DestroyVoice();
        m_masteringVoice = nullptr;
    }

    if (m_xaudio2)
    {
        m_xaudio2->Release();
        m_xaudio2 = nullptr;
    }

    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
}

// Loads a PCM WAV file and creates a source voice for it. Returns a stem
// handle, or an invalid one on failure (missing file or unsupported format).
StemHandle AudioEngine::LoadStem(const std::wstring& wavFilePath)
{
    if (!m_xaudio2)
    {
        return StemHandle{};
    }

    Stem stem;
    if (!LoadWavFile(wavFilePath, stem.pcmData, stem.format))
    {
        return StemHandle{};
    }

    if (FAILED(m_xaudio2->CreateSourceVoice(&stem.voice, &stem.format)) || !stem.voice)
    {
        return StemHandle{};
    }

    stem.wavFilePath = wavFilePath;
    m_stems.push_back(std::move(stem));
    return StemHandle{static_cast<int>(m_stems.size()) - 1};
}

// Asserts if any OTHER loaded stem sharing playingHandle's wavFilePath is
// currently audible - see StartLooping's own call site comment for why this
// exists and why it's safe against a stem restarting its own voice.
void AudioEngine::AssertNoOtherStemForSameFilePlaying(StemHandle playingHandle) const
{
    const Stem& playing = m_stems[playingHandle.value];
    for (size_t i = 0; i < m_stems.size(); ++i)
    {
        if (static_cast<int>(i) == playingHandle.value || !m_stems[i].voice)
        {
            continue;
        }
        if (m_stems[i].wavFilePath != playing.wavFilePath)
        {
            continue;
        }

        XAUDIO2_VOICE_STATE otherState{};
        m_stems[i].voice->GetState(&otherState);
        if (otherState.BuffersQueued > 0)
        {
            fwprintf(stderr,
                     L"AudioEngine: '%ls' is about to play on stem %d while stem %zu of the same file is "
                     L"still audible - the same .wav would be playing on top of itself.\n",
                     playing.wavFilePath.c_str(), playingHandle.value, i);
            assert(false && "AudioEngine: the same .wav file is being started while another stem of it is already playing");
        }
    }
}

// Starts a loaded stem looping seamlessly, seeking to phaseSeconds so it enters in time with the beat grid.
void AudioEngine::StartLooping(StemHandle stemHandle, double phaseSeconds, float volume, int loopCount)
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    Stem& stem = m_stems[stemHandle.value];
    if (!stem.voice || stem.pcmData.empty() || stem.format.nBlockAlign == 0)
    {
        return;
    }

    // Restarting THIS stem's own voice (Stop/Flush/resubmit, right below) is
    // normal and not a violation - the invariant this guards is "at most one
    // voice for a given .wav file is audible at once," which only a
    // DIFFERENT stem loaded from the same file (e.g. a chart bug, or a
    // leaked stem from an earlier LoadChart) could break.
    AssertNoOtherStemForSameFilePlaying(stemHandle);

    stem.voice->Stop();
    stem.voice->FlushSourceBuffers();

    UINT32 totalFrames = GetTotalFrames(stem);
    UINT32 startFrame = 0;
    if (totalFrames > 0 && stem.format.nSamplesPerSec > 0)
    {
        double loopDurationSeconds = static_cast<double>(totalFrames) / stem.format.nSamplesPerSec;
        double wrappedPhase = std::fmod(phaseSeconds, loopDurationSeconds);
        if (wrappedPhase < 0.0)
        {
            wrappedPhase += loopDurationSeconds;
        }
        startFrame = static_cast<UINT32>(wrappedPhase * stem.format.nSamplesPerSec);
        if (startFrame >= totalFrames)
        {
            startFrame = totalFrames - 1;
        }
    }

    // Play once from startFrame to the end of the buffer (entering in phase),
    // then loop the whole buffer forever - so the audio lands exactly in
    // time with the beat grid rather than restarting from sample 0.
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(stem.pcmData.size());
    buffer.pAudioData = stem.pcmData.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.PlayBegin = startFrame;
    buffer.PlayLength = totalFrames - startFrame;
    buffer.LoopBegin = 0;
    buffer.LoopLength = 0;
    // A finite loopCount is expressed to XAudio2 as (loopCount - 1): the
    // initial PlayBegin/PlayLength pass already accounts for one full play
    // of the region, so LoopCount here only counts the ADDITIONAL repeats
    // of the [0, end) loop region after that.
    buffer.LoopCount = loopCount > 0 ? static_cast<UINT32>(loopCount - 1) : XAUDIO2_LOOP_INFINITE;

    stem.voice->SetVolume(volume);
    stem.voice->SubmitSourceBuffer(&buffer);
    stem.voice->Start();

    // SamplesPlayed is a lifetime counter for the voice, not reset by Start/Stop/
    // Flush, so we record a baseline here to measure "seconds since this loop began."
    XAUDIO2_VOICE_STATE state{};
    stem.voice->GetState(&state);
    stem.loopStartSampleBaseline = state.SamplesPlayed;
}

// Changes the volume of an already-playing (or not-yet-playing) stem without otherwise affecting playback.
void AudioEngine::SetVolume(StemHandle stemHandle, float volume)
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    Stem& stem = m_stems[stemHandle.value];
    if (stem.voice)
    {
        stem.voice->SetVolume(volume);
    }
}

// Ramps every voice in stems down together, then stops/flushes all of them -
// see the header's own comment for why this exists instead of a bare
// Stop()+FlushSourceBuffers() per voice.
void AudioEngine::FadeOutAndStop(const std::vector<Stem*>& stems)
{
    if (stems.empty())
    {
        return;
    }

    // Read each voice's own current volume once, up front - a stem mid-
    // Learn-section's own init_volume/volume split, or a break clip at
    // less than unity gain, must fade from wherever it actually is, not
    // from 1.0, or this would audibly jump louder for an instant before
    // fading down.
    std::vector<float> startVolumes(stems.size(), 0.0f);
    for (size_t i = 0; i < stems.size(); ++i)
    {
        if (stems[i]->voice)
        {
            stems[i]->voice->GetVolume(&startVolumes[i]);
        }
    }

    // A handful of small, evenly-spaced volume steps rather than one jump
    // straight to silence - XAudio2's Stop() truncates a voice's waveform
    // at whatever sample it's currently on, which is an audible click on
    // its own, and considerably more so when several simultaneously-
    // stopped voices (a Break/Reset's own stop-everything, in particular)
    // all truncate at once. ~10ms total is short enough that even a
    // fast-decaying note reads as a clean stop, not a lingering fade.
    constexpr int c_FadeSteps = 5;
    constexpr DWORD c_FadeStepMs = 2;
    for (int step = 1; step <= c_FadeSteps; ++step)
    {
        float remaining = 1.0f - static_cast<float>(step) / static_cast<float>(c_FadeSteps);
        for (size_t i = 0; i < stems.size(); ++i)
        {
            if (stems[i]->voice)
            {
                stems[i]->voice->SetVolume(startVolumes[i] * remaining);
            }
        }
        Sleep(c_FadeStepMs);
    }

    for (Stem* stem : stems)
    {
        if (stem->voice)
        {
            stem->voice->Stop();
            stem->voice->FlushSourceBuffers();
        }
    }
}

// Stops a single stem.
void AudioEngine::Stop(StemHandle stemHandle)
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    FadeOutAndStop({&m_stems[stemHandle.value]});
}

// Stops every currently loaded stem.
void AudioEngine::StopAll()
{
    std::vector<Stem*> stems;
    stems.reserve(m_stems.size());
    for (Stem& stem : m_stems)
    {
        stems.push_back(&stem);
    }
    FadeOutAndStop(stems);
}

// Stops every currently loaded stem except keep - see the header's own comment.
void AudioEngine::StopAllExcept(StemHandle keep)
{
    std::vector<Stem*> stems;
    stems.reserve(m_stems.size());
    for (size_t i = 0; i < m_stems.size(); ++i)
    {
        if (static_cast<int>(i) != keep.value)
        {
            stems.push_back(&m_stems[i]);
        }
    }
    FadeOutAndStop(stems);
}

// Pauses every currently loaded stem in place, without flushing its queued
// buffer.
void AudioEngine::PauseAll()
{
    for (Stem& stem : m_stems)
    {
        if (stem.voice)
        {
            stem.voice->Stop();
        }
    }
}

// Resumes every stem paused by PauseAll() - Start() on a voice whose buffer
// is still queued (never flushed) simply continues submitting from wherever
// XAudio2 left off, with no reseek needed.
void AudioEngine::ResumeAll()
{
    for (Stem& stem : m_stems)
    {
        if (stem.voice)
        {
            stem.voice->Start();
        }
    }
}

// Returns a stem's current voice volume (1.0 = unity gain), or -1.0 if the handle is invalid.
float AudioEngine::GetVolume(StemHandle stemHandle) const
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return -1.0f;
    }

    const Stem& stem = m_stems[stemHandle.value];
    if (!stem.voice)
    {
        return -1.0f;
    }

    float volume = 0.0f;
    stem.voice->GetVolume(&volume);
    return volume;
}

// Returns how many seconds of audio have played for a stem, for clock resync.
double AudioEngine::GetPositionSeconds(StemHandle stemHandle) const
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return 0.0;
    }

    const Stem& stem = m_stems[stemHandle.value];
    if (!stem.voice || stem.format.nSamplesPerSec == 0)
    {
        return 0.0;
    }

    XAUDIO2_VOICE_STATE state{};
    stem.voice->GetState(&state);
    UINT64 samplesSinceLoopStart = state.SamplesPlayed - stem.loopStartSampleBaseline;
    return static_cast<double>(samplesSinceLoopStart) / stem.format.nSamplesPerSec;
}

// Returns whether a stem's voice is still actively producing audio.
bool AudioEngine::IsPlaying(StemHandle stemHandle) const
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return false;
    }

    const Stem& stem = m_stems[stemHandle.value];
    if (!stem.voice)
    {
        return false;
    }

    XAUDIO2_VOICE_STATE state{};
    stem.voice->GetState(&state);
    return state.BuffersQueued > 0;
}

// Returns a stem's total duration in seconds, measured from its actual loaded audio data.
double AudioEngine::GetStemDurationSeconds(StemHandle stemHandle) const
{
    if (!stemHandle.IsValid() || stemHandle.value >= static_cast<int>(m_stems.size()))
    {
        return 0.0;
    }

    const Stem& stem = m_stems[stemHandle.value];
    if (stem.format.nSamplesPerSec == 0)
    {
        return 0.0;
    }

    return static_cast<double>(GetTotalFrames(stem)) / stem.format.nSamplesPerSec;
}

// Loads a short PCM WAV file for one-shot playback - see the header's own comment.
SfxHandle AudioEngine::LoadSfx(const std::wstring& wavFilePath)
{
    if (!m_xaudio2)
    {
        return SfxHandle{};
    }

    Sfx sfx;
    if (!LoadWavFile(wavFilePath, sfx.pcmData, sfx.format))
    {
        return SfxHandle{};
    }

    for (int i = 0; i < c_SfxVoicePoolSize; ++i)
    {
        if (FAILED(m_xaudio2->CreateSourceVoice(&sfx.voices[i], &sfx.format)) || !sfx.voices[i])
        {
            for (int created = 0; created < i; ++created)
            {
                sfx.voices[created]->DestroyVoice();
            }
            return SfxHandle{};
        }
    }

    m_sfx.push_back(std::move(sfx));
    return SfxHandle{static_cast<int>(m_sfx.size()) - 1};
}

// Plays sfx once, fire-and-forget, from the next voice in its pool - see the header's own comment.
void AudioEngine::PlaySfx(SfxHandle sfx)
{
    if (!sfx.IsValid() || sfx.value >= static_cast<int>(m_sfx.size()))
    {
        return;
    }

    Sfx& entry = m_sfx[sfx.value];
    if (entry.pcmData.empty() || entry.format.nBlockAlign == 0)
    {
        return;
    }

    IXAudio2SourceVoice* voice = entry.voices[entry.nextVoice];
    entry.nextVoice = (entry.nextVoice + 1) % c_SfxVoicePoolSize;
    if (!voice)
    {
        return;
    }

    voice->Stop();
    voice->FlushSourceBuffers();

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(entry.pcmData.size());
    buffer.pAudioData = entry.pcmData.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount = 0;

    voice->SetVolume(1.0f);
    voice->SubmitSourceBuffer(&buffer);
    voice->Start();
}

// Returns a stem's total length in sample frames, derived from its PCM data size.
UINT32 AudioEngine::GetTotalFrames(const Stem& stem)
{
    if (stem.format.nBlockAlign == 0)
    {
        return 0;
    }
    return static_cast<UINT32>(stem.pcmData.size() / stem.format.nBlockAlign);
}
