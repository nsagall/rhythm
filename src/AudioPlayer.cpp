#include "AudioPlayer.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::Play(const std::wstring& filePath, int repeatCount) {
    Stop();

    repeatCount = std::max(repeatCount, 1);
    m_stopRequested = false;
    m_thread = std::thread(&AudioPlayer::PlaybackThreadProc, this, filePath, repeatCount);
    return true;
}

void AudioPlayer::PlaybackThreadProc(std::wstring filePath, int repeatCount) {
    for (int i = 0; i < repeatCount && !m_stopRequested; ++i) {
        PlaySoundW(filePath.c_str(), nullptr, SND_FILENAME | SND_SYNC);
    }
}

void AudioPlayer::Stop() {
    m_stopRequested = true;
    PlaySoundW(nullptr, nullptr, 0);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}
