#pragma once

#include <string>

constexpr int kTrackCount = 5;

struct WavTrack {
    std::wstring filePath;
    int repeatCount = 1;
};
