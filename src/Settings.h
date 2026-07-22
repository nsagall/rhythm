#pragma once

#include <array>

#include "WavTrack.h"

class Settings {
public:
    std::array<WavTrack, kTrackCount> Load();
    void Save(const std::array<WavTrack, kTrackCount>& tracks);
};
