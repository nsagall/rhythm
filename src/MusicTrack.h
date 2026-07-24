#pragma once

#include <string>

constexpr int kTrackCount = 1;

// A single audio file (.wav or .mp3) plus its playback settings.
struct MusicTrack
{
    std::wstring filePath;
    int repeatCount = 1;
    int bpm = 120;
};
