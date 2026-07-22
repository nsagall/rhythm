#pragma once

#include <atomic>
#include <string>
#include <thread>

class AudioPlayer {
public:
    ~AudioPlayer();

    bool Play(const std::wstring& filePath, int repeatCount = 1);
    void Stop();

private:
    void PlaybackThreadProc(std::wstring filePath, int repeatCount);

    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
};
