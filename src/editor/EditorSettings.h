#pragma once

#include <windows.h>

#include <string>

// Persists RhythmEditor's preferences between runs, backed by editor_settings.ini under
// %APPDATA%\Rhythm - separate from the game's settings.ini.
class EditorSettings
{
public:
    // Reads the last-opened chart path (empty if none saved).
    std::wstring LoadLastChartPath();

    // Saves the given chart path so it's pre-filled next launch.
    void SaveLastChartPath(const std::wstring& chartPath);

    // Generic persistence for pane-splitter positions (pixels) - one value per key under the
    // "Layout" section. Returns defaultValue if key was never saved.
    float LoadPaneSize(const wchar_t* key, float defaultValue);
    void SavePaneSize(const wchar_t* key, float value);

    // Loads the last-saved main window placement into outPlacement (with .length set for a
    // SetWindowPlacement call). Returns false, leaving outPlacement untouched, if nothing was saved
    // or the rect is degenerate.
    bool LoadWindowPlacement(WINDOWPLACEMENT& outPlacement);

    // Saves the given window placement so it's restored next launch.
    void SaveWindowPlacement(const WINDOWPLACEMENT& placement);
};
