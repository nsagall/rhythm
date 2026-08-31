#include "EditorSettings.h"

#include <windows.h>

#include <cstdlib>
#include <cwchar>

namespace
{

// Returns the path to editor_settings.ini under %APPDATA%\Rhythm, creating the folder if needed. A
// distinct file from the game's settings.ini in the same directory.
std::wstring GetSettingsFilePath()
{
    wchar_t appDataDir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appDataDir, MAX_PATH);

    std::wstring dir = std::wstring(appDataDir) + L"\\Rhythm";
    CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\editor_settings.ini";
}

} // namespace

std::wstring EditorSettings::LoadLastChartPath()
{
    wchar_t buffer[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"App", L"LastChartPath", L"", buffer, MAX_PATH, GetSettingsFilePath().c_str());
    return buffer;
}

void EditorSettings::SaveLastChartPath(const std::wstring& chartPath)
{
    WritePrivateProfileStringW(L"App", L"LastChartPath", chartPath.c_str(), GetSettingsFilePath().c_str());
}

float EditorSettings::LoadPaneSize(const wchar_t* key, float defaultValue)
{
    wchar_t defaultBuf[64];
    swprintf(defaultBuf, 64, L"%.3f", defaultValue);
    wchar_t buffer[64] = L"";
    GetPrivateProfileStringW(L"Layout", key, defaultBuf, buffer, 64, GetSettingsFilePath().c_str());
    return static_cast<float>(_wtof(buffer));
}

void EditorSettings::SavePaneSize(const wchar_t* key, float value)
{
    wchar_t buffer[64];
    swprintf(buffer, 64, L"%.3f", value);
    WritePrivateProfileStringW(L"Layout", key, buffer, GetSettingsFilePath().c_str());
}

bool EditorSettings::LoadWindowPlacement(WINDOWPLACEMENT& outPlacement)
{
    wchar_t buffer[128] = L"";
    GetPrivateProfileStringW(L"Window", L"Placement", L"", buffer, 128, GetSettingsFilePath().c_str());
    if (buffer[0] == L'\0')
    {
        return false;
    }

    LONG left, top, right, bottom;
    int maximized = 0;
    if (swscanf(buffer, L"%ld,%ld,%ld,%ld,%d", &left, &top, &right, &bottom, &maximized) != 5)
    {
        return false;
    }
    if (right <= left || bottom <= top)
    {
        return false;
    }

    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(WINDOWPLACEMENT);
    placement.showCmd = maximized != 0 ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    placement.rcNormalPosition = RECT{left, top, right, bottom};
    outPlacement = placement;
    return true;
}

void EditorSettings::SaveWindowPlacement(const WINDOWPLACEMENT& placement)
{
    wchar_t buffer[128];
    swprintf(buffer, 128, L"%ld,%ld,%ld,%ld,%d", placement.rcNormalPosition.left, placement.rcNormalPosition.top,
              placement.rcNormalPosition.right, placement.rcNormalPosition.bottom,
              placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    WritePrivateProfileStringW(L"Window", L"Placement", buffer, GetSettingsFilePath().c_str());
}
