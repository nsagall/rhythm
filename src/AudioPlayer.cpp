#include "AudioPlayer.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::PlayAll(std::vector<WavTrack> tracks) {
    Stop();

    m_stopRequested = false;
    m_thread = std::thread(&AudioPlayer::PlaybackThreadProc, this, std::move(tracks));
    return true;
}

void AudioPlayer::PlaybackThreadProc(std::vector<WavTrack> tracks) {
    for (const WavTrack& track : tracks) {
        if (m_stopRequested) {
            break;
        }

        int repeatCount = std::max(track.repeatCount, 1);
        for (int i = 0; i < repeatCount && !m_stopRequested; ++i) {
            PlaySoundW(track.filePath.c_str(), nullptr, SND_FILENAME | SND_SYNC);
        }
    }
}

void AudioPlayer::Stop() {
    m_stopRequested = true;
    PlaySoundW(nullptr, nullptr, 0);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}
