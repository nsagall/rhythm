#pragma once

#include <string>

constexpr int kTrackCount = 5;

struct MusicTrack {
    std::wstring filePath;
    int repeatCount = 1;
    int bpm = 120;
};
