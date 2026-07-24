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
};
