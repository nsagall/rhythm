#pragma once

#include <string>

// Persists app-level preferences between runs, backed by an INI file under %APPDATA%\Rhythm.
class Settings
{
public:
    // Reads the saved last-chart path from disk (empty if none saved yet).
    std::wstring LoadLastChartPath();

    // Saves the given chart path so it's pre-filled next launch.
    void SaveLastChartPath(const std::wstring& chartPath);

    // Reads the saved Easy Mode toggle state from disk (false if none saved yet).
    bool LoadEasyMode();

    // Saves the Easy Mode toggle state so it's restored next launch.
    void SaveEasyMode(bool easyMode);

    // Reads lane index `lane`'s saved custom input binding, serialized by
    // LaneBindings (see src/LaneBindings.h) - Settings itself doesn't know or
    // care what the string means, only that it round-trips. Empty if none
    // saved yet.
    std::wstring LoadLaneBinding(int lane);

    // Saves lane index `lane`'s custom input binding, already serialized by
    // LaneBindings, so it's restored next launch.
    void SaveLaneBinding(int lane, const std::wstring& serialized);
};
