#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
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
// handle, or -1 on failure (missing file or unsupported format).
int AudioEngine::LoadStem(const std::wstring& wavFilePath)
{
    if (!m_xaudio2)
    {
        return -1;
    }

    Stem stem;
    if (!LoadWavFile(wavFilePath, stem.pcmData, stem.format))
    {
        return -1;
    }

    if (FAILED(m_xaudio2->CreateSourceVoice(&stem.voice, &stem.format)) || !stem.voice)
    {
        return -1;
    }

    m_stems.push_back(std::move(stem));
    return static_cast<int>(m_stems.size()) - 1;
}

// Starts a loaded stem looping seamlessly, seeking to phaseSeconds so it enters in time with the beat grid.
void AudioEngine::StartLooping(int stemHandle, double phaseSeconds)
{
    if (stemHandle < 0 || stemHandle >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    Stem& stem = m_stems[stemHandle];
    if (!stem.voice || stem.pcmData.empty() || stem.format.nBlockAlign == 0)
    {
        return;
    }

    stem.voice->Stop();
    stem.voice->FlushSourceBuffers();

    UINT32 totalFrames = static_cast<UINT32>(stem.pcmData.size() / stem.format.nBlockAlign);
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
    buffer.LoopCount = XAUDIO2_LOOP_INFINITE;

    stem.voice->SubmitSourceBuffer(&buffer);
    stem.voice->Start();

    // SamplesPlayed is a lifetime counter for the voice, not reset by Start/Stop/
    // Flush, so we record a baseline here to measure "seconds since this loop began."
    XAUDIO2_VOICE_STATE state{};
    stem.voice->GetState(&state);
    stem.loopStartSampleBaseline = state.SamplesPlayed;
}

// Stops a single stem.
void AudioEngine::Stop(int stemHandle)
{
    if (stemHandle < 0 || stemHandle >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    Stem& stem = m_stems[stemHandle];
    if (stem.voice)
    {
        stem.voice->Stop();
        stem.voice->FlushSourceBuffers();
    }
}

// Stops every currently loaded stem.
void AudioEngine::StopAll()
{
    for (Stem& stem : m_stems)
    {
        if (stem.voice)
        {
            stem.voice->Stop();
            stem.voice->FlushSourceBuffers();
        }
    }
}

// Returns how many seconds of audio have played for a stem, for clock resync.
double AudioEngine::GetPositionSeconds(int stemHandle) const
{
    if (stemHandle < 0 || stemHandle >= static_cast<int>(m_stems.size()))
    {
        return 0.0;
    }

    const Stem& stem = m_stems[stemHandle];
    if (!stem.voice || stem.format.nSamplesPerSec == 0)
    {
        return 0.0;
    }

    XAUDIO2_VOICE_STATE state{};
    stem.voice->GetState(&state);
    UINT64 samplesSinceLoopStart = state.SamplesPlayed - stem.loopStartSampleBaseline;
    return static_cast<double>(samplesSinceLoopStart) / stem.format.nSamplesPerSec;
}
