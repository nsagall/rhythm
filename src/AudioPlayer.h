#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "MusicTrack.h"

// Plays a set of MusicTracks concurrently (one MCI device per track) and
// reports per-track start/stop so the UI can react (e.g. drive an
// animation), without knowing anything about Win32 windows itself.
class AudioPlayer
{
public:
    using TrackStateCallback = std::function<void(int trackIndex, bool isPlaying)>;

    ~AudioPlayer();

    // Registers the callback invoked (from a background thread) when a
    // track starts or stops playing.
    void SetTrackStateCallback(TrackStateCallback callback);

    // Stops any current playback, then plays every track with a non-empty
    // file path concurrently, each repeated its own repeatCount times.
    bool PlayAll(std::vector<MusicTrack> tracks);

    // Stops all playback and blocks until every playback thread has exited.
    void Stop();

private:
    // Background-thread body: opens one track's MCI device, plays it
    // repeatCount times (polling for the stop flag between plays), then closes it.
    void PlayTrackThreadProc(MusicTrack track, int slotIndex);

    TrackStateCallback m_onTrackStateChanged;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stopRequested{false};
};
