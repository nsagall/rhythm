#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "WavTrack.h"

class AudioPlayer {
public:
    ~AudioPlayer();

    bool PlayAll(std::vector<WavTrack> tracks);
    void Stop();

private:
    void PlaybackThreadProc(std::vector<WavTrack> tracks);

    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
};
