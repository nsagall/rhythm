#pragma once

#include <array>

#include "MusicTrack.h"

// Persists track settings between runs, backed by an INI file under %APPDATA%\Rhythm.
class Settings
{
public:
    // Reads saved tracks from disk, defaulting any missing values.
    std::array<MusicTrack, kTrackCount> Load();

    // Writes the given tracks to disk, overwriting any previous save.
    void Save(const std::array<MusicTrack, kTrackCount>& tracks);
};
