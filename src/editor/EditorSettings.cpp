#include "EditorSettings.h"

#include <windows.h>

namespace
{

// Returns the path to editor_settings.ini under %APPDATA%\Rhythm, creating
// the folder if needed. Same %APPDATA%\Rhythm directory the game's own
// Settings class uses, but a distinct file, so the two never step on each
// other's keys.
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
