#include "AudioEngine.h"

#include <algorithm>
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

// Synthesizes a new stem by placing copies of a source stem's audio at
// each onset within a repeating span, silence-padded to fill exactly one
// span at the given tempo.
int AudioEngine::BuildPatternLoop(int sourceStemHandle, const std::vector<double>& onsetBeats, double spanBeats, double bpm)
{
    if (!m_xaudio2 || sourceStemHandle < 0 || sourceStemHandle >= static_cast<int>(m_stems.size()))
    {
        return -1;
    }

    const Stem& source = m_stems[sourceStemHandle];
    if (!source.voice || source.pcmData.empty() || source.format.wBitsPerSample != 16)
    {
        return -1;
    }

    double secondsPerBeat = 60.0 / bpm;
    double spanSeconds = spanBeats * secondsPerBeat;
    UINT32 bytesPerFrame = (source.format.wBitsPerSample / 8) * source.format.nChannels;
    UINT32 totalFrames = static_cast<UINT32>(spanSeconds * source.format.nSamplesPerSec);
    UINT32 totalBytes = totalFrames * bytesPerFrame;

    Stem loopStem;
    loopStem.format = source.format;
    loopStem.pcmData.assign(totalBytes, 0);

    for (double onsetBeat : onsetBeats)
    {
        double onsetSeconds = onsetBeat * secondsPerBeat;
        UINT32 startFrame = static_cast<UINT32>(onsetSeconds * source.format.nSamplesPerSec);
        UINT32 startByte = startFrame * bytesPerFrame;
        if (startByte >= totalBytes)
        {
            continue;
        }

        UINT32 copyBytes = std::min<UINT32>(static_cast<UINT32>(source.pcmData.size()), totalBytes - startByte);
        UINT32 sampleCount = copyBytes / sizeof(INT16);
        const INT16* src = reinterpret_cast<const INT16*>(source.pcmData.data());
        INT16* dst = reinterpret_cast<INT16*>(loopStem.pcmData.data() + startByte);
        for (UINT32 i = 0; i < sampleCount; ++i)
        {
            int mixed = static_cast<int>(dst[i]) + static_cast<int>(src[i]);
            dst[i] = static_cast<INT16>(std::clamp(mixed, -32768, 32767));
        }
    }

    if (FAILED(m_xaudio2->CreateSourceVoice(&loopStem.voice, &loopStem.format)) || !loopStem.voice)
    {
        return -1;
    }

    m_stems.push_back(std::move(loopStem));
    return static_cast<int>(m_stems.size()) - 1;
}

// Submits pcmData as a buffer (optionally looping) and starts playback.
void AudioEngine::SubmitAndPlay(Stem& stem, bool loop)
{
    if (!stem.voice)
    {
        return;
    }

    stem.voice->Stop();
    stem.voice->FlushSourceBuffers();

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(stem.pcmData.size());
    buffer.pAudioData = stem.pcmData.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (loop)
    {
        buffer.LoopBegin = 0;
        buffer.LoopLength = 0;
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    }

    stem.voice->SubmitSourceBuffer(&buffer);
    stem.voice->Start();
}

// Plays a loaded stem once, from the start, without looping.
void AudioEngine::PlayOneShot(int stemHandle)
{
    if (stemHandle < 0 || stemHandle >= static_cast<int>(m_stems.size()))
    {
        return;
    }
    SubmitAndPlay(m_stems[stemHandle], /*loop=*/false);
}

// Starts a loaded stem looping seamlessly, from the start.
void AudioEngine::StartLooping(int stemHandle)
{
    if (stemHandle < 0 || stemHandle >= static_cast<int>(m_stems.size()))
    {
        return;
    }

    Stem& stem = m_stems[stemHandle];
    SubmitAndPlay(stem, /*loop=*/true);

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
