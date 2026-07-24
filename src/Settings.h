#pragma once

#include <array>

#include "MusicTrack.h"

class Settings {
public:
    std::array<MusicTrack, kTrackCount> Load();
    void Save(const std::array<MusicTrack, kTrackCount>& tracks);
};
