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

    // Generic persistence for resizable-pane layout (splitter positions in
    // EditorApp, in pixels) - one value per caller-chosen key under the
    // "Layout" section, so each new splitter doesn't need its own bespoke
    // Load/Save method. Returns defaultValue if key has never been saved.
    float LoadPaneSize(const wchar_t* key, float defaultValue);
    void SavePaneSize(const wchar_t* key, float value);
};
