#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "MusicTrack.h"

class AudioPlayer {
public:
    using TrackStateCallback = std::function<void(int trackIndex, bool isPlaying)>;

    ~AudioPlayer();

    void SetTrackStateCallback(TrackStateCallback callback);

    bool PlayAll(std::vector<MusicTrack> tracks);
    void Stop();

private:
    void PlayTrackThreadProc(MusicTrack track, int slotIndex);

    TrackStateCallback m_onTrackStateChanged;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stopRequested{false};
};
