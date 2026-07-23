#include "AudioPlayer.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <string>

namespace {

constexpr DWORD kStatusPollIntervalMs = 50;

std::wstring MakeAlias(int slotIndex) {
    wchar_t alias[32];
    wsprintfW(alias, L"rhythmtrack%d", slotIndex);
    return alias;
}

} // namespace

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::PlayAll(std::vector<WavTrack> tracks) {
    Stop();

    m_stopRequested = false;
    for (size_t i = 0; i < tracks.size(); ++i) {
        m_threads.emplace_back(&AudioPlayer::PlayTrackThreadProc, this, tracks[i], static_cast<int>(i));
    }
    return true;
}

void AudioPlayer::PlayTrackThreadProc(WavTrack track, int slotIndex) {
    std::wstring alias = MakeAlias(slotIndex);

    // No explicit "type" here: MCI resolves the device driver from the file's
    // extension (waveaudio for .wav, mciqtz32/DirectShow for .mp3 and others).
    std::wstring openCmd = L"open \"" + track.filePath + L"\" alias " + alias;
    if (mciSendStringW(openCmd.c_str(), nullptr, 0, nullptr) != 0) {
        return;
    }

    std::wstring playCmd = L"play " + alias + L" from 0";
    std::wstring statusCmd = L"status " + alias + L" mode";

    // Some MCI drivers (e.g. the DirectShow-backed one used for .mp3) don't
    // reliably unblock a synchronous "wait" when "stop" is sent from another
    // thread, which stalls shutdown for the length of the track. Polling our
    // own stop flag keeps Stop() responsive regardless of the backing driver.
    int repeatCount = std::max(track.repeatCount, 1);
    for (int i = 0; i < repeatCount && !m_stopRequested; ++i) {
        mciSendStringW(playCmd.c_str(), nullptr, 0, nullptr);

        wchar_t statusBuf[64] = L"";
        while (!m_stopRequested) {
            if (mciSendStringW(statusCmd.c_str(), statusBuf, 64, nullptr) != 0) {
                break;
            }
            if (wcscmp(statusBuf, L"playing") != 0) {
                break;
            }
            Sleep(kStatusPollIntervalMs);
        }
    }

    std::wstring stopCmd = L"stop " + alias;
    mciSendStringW(stopCmd.c_str(), nullptr, 0, nullptr);
    std::wstring closeCmd = L"close " + alias;
    mciSendStringW(closeCmd.c_str(), nullptr, 0, nullptr);
}

void AudioPlayer::Stop() {
    m_stopRequested = true;

    for (std::thread& t : m_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_threads.clear();
}
