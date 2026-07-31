#pragma once

#include <string>

// Persists RhythmEditor's own preferences between runs, backed by its own
// INI file under %APPDATA%\Rhythm (editor_settings.ini) - deliberately
// separate from the game's Settings class/settings.ini so editor work never
// touches the game's last-chart/Easy Mode persistence.
class EditorSettings
{
public:
    // Reads the last-opened chart path from disk (empty if none saved yet).
    std::wstring LoadLastChartPath();

    // Saves the given chart path so it's pre-filled next launch.
    void SaveLastChartPath(const std::wstring& chartPath);
};
