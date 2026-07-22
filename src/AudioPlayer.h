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
    void PlayTrackThreadProc(WavTrack track, int slotIndex);

    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stopRequested{false};
};
