#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cstdio>
#include <cmath>
#include <vector>

#include "AudioEngine.h"

// Standalone diagnostic (not part of the normal build): plays one stem at
// two different volumes while capturing the actual rendered output via
// WASAPI loopback, then compares peak levels - a definitive, objective
// test of whether AudioEngine::SetVolume actually changes what comes out
// of the speakers, independent of human hearing or GetVolume() readback
// (which only proves the requested value was stored, not that it's audible).

namespace
{

float CapturePeakWhilePlaying(AudioEngine& engine, StemHandle handle, float volume, int captureMs)
{
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr))
    {
        printf("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx\n", hr);
        return -1.0f;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr))
    {
        printf("GetDefaultAudioEndpoint failed: 0x%08lx\n", hr);
        return -1.0f;
    }

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    device->Release();
    if (FAILED(hr))
    {
        printf("Activate(IAudioClient) failed: 0x%08lx\n", hr);
        return -1.0f;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    audioClient->GetMixFormat(&mixFormat);

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 20000000, 0, mixFormat, nullptr);
    if (FAILED(hr))
    {
        printf("audioClient->Initialize failed: 0x%08lx\n", hr);
        CoTaskMemFree(mixFormat);
        audioClient->Release();
        return -1.0f;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr))
    {
        printf("GetService(IAudioCaptureClient) failed: 0x%08lx\n", hr);
        CoTaskMemFree(mixFormat);
        audioClient->Release();
        return -1.0f;
    }

    audioClient->Start();

    // Start the stem playing at the requested volume, then capture for captureMs.
    engine.StartLooping(handle, 0.0, volume);

    float peak = 0.0f;
    bool isFloatFormat = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                          (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE); // assume float for EXTENSIBLE mix format (typical)

    DWORD startTick = GetTickCount();
    while (GetTickCount() - startTick < static_cast<DWORD>(captureMs))
    {
        Sleep(10);

        UINT32 packetLength = 0;
        captureClient->GetNextPacketSize(&packetLength);
        while (packetLength != 0)
        {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr))
            {
                break;
            }

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data != nullptr)
            {
                UINT32 sampleCount = framesAvailable * mixFormat->nChannels;
                if (isFloatFormat)
                {
                    const float* samples = reinterpret_cast<const float*>(data);
                    for (UINT32 i = 0; i < sampleCount; ++i)
                    {
                        float mag = std::fabs(samples[i]);
                        if (mag > peak)
                        {
                            peak = mag;
                        }
                    }
                }
            }

            captureClient->ReleaseBuffer(framesAvailable);
            captureClient->GetNextPacketSize(&packetLength);
        }
    }

    engine.Stop(handle);
    audioClient->Stop();

    CoTaskMemFree(mixFormat);
    captureClient->Release();
    audioClient->Release();

    return peak;
}

} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("AudioEngine::Initialize failed\n");
        return 1;
    }

    StemHandle handle = engine.LoadStem(L"Content/Cool Boy/stems/phee.wav");
    if (!handle.IsValid())
    {
        printf("LoadStem failed\n");
        return 1;
    }

    printf("Capturing real output peak at volume=1.0 ...\n");
    float peakAt1 = CapturePeakWhilePlaying(engine, handle, 1.0f, 1500);
    printf("  peak amplitude at volume=1.0: %.5f\n\n", peakAt1);

    Sleep(300);

    printf("Capturing real output peak at volume=3.0 ...\n");
    float peakAt3 = CapturePeakWhilePlaying(engine, handle, 3.0f, 1500);
    printf("  peak amplitude at volume=3.0: %.5f\n\n", peakAt3);

    printf("Ratio (3.0-run peak / 1.0-run peak): %.3f (expect roughly 3.0, or less if clipping at 1.0)\n",
           peakAt1 > 0.0001f ? (peakAt3 / peakAt1) : -1.0f);

    engine.Shutdown();
    CoUninitialize();
    return 0;
}
