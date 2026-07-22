#include "AudioPlayer.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <string>

namespace {

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

    std::wstring openCmd = L"open \"" + track.filePath + L"\" type waveaudio alias " + alias;
    if (mciSendStringW(openCmd.c_str(), nullptr, 0, nullptr) != 0) {
        return;
    }

    std::wstring playCmd = L"play " + alias + L" from 0 wait";
    int repeatCount = std::max(track.repeatCount, 1);
    for (int i = 0; i < repeatCount && !m_stopRequested; ++i) {
        mciSendStringW(playCmd.c_str(), nullptr, 0, nullptr);
    }

    std::wstring closeCmd = L"close " + alias;
    mciSendStringW(closeCmd.c_str(), nullptr, 0, nullptr);
}

void AudioPlayer::Stop() {
    m_stopRequested = true;

    for (int i = 0; i < kTrackCount; ++i) {
        std::wstring stopCmd = L"stop " + MakeAlias(i);
        mciSendStringW(stopCmd.c_str(), nullptr, 0, nullptr);
    }

    for (std::thread& t : m_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_threads.clear();
}
