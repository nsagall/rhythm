#include "Settings.h"

#include <windows.h>

namespace
{

// Returns the path to settings.ini under %APPDATA%\Rhythm, creating the folder if needed.
std::wstring GetSettingsFilePath()
{
    wchar_t appDataDir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appDataDir, MAX_PATH);

    std::wstring dir = std::wstring(appDataDir) + L"\\Rhythm";
    CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\settings.ini";
}

} // namespace

// Reads the saved last-chart path from disk (empty if none saved yet).
std::wstring Settings::LoadLastChartPath()
{
    wchar_t buffer[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"App", L"LastChartPath", L"", buffer, MAX_PATH, GetSettingsFilePath().c_str());
    return buffer;
}

// Saves the given chart path so it's pre-filled next launch.
void Settings::SaveLastChartPath(const std::wstring& chartPath)
{
    WritePrivateProfileStringW(L"App", L"LastChartPath", chartPath.c_str(), GetSettingsFilePath().c_str());
}

// Reads the saved Easy Mode toggle state from disk (false if none saved yet).
bool Settings::LoadEasyMode()
{
    return GetPrivateProfileIntW(L"App", L"EasyMode", 0, GetSettingsFilePath().c_str()) != 0;
}

// Saves the Easy Mode toggle state so it's restored next launch.
void Settings::SaveEasyMode(bool easyMode)
{
    WritePrivateProfileStringW(L"App", L"EasyMode", easyMode ? L"1" : L"0", GetSettingsFilePath().c_str());
}

// Reads lane index `lane`'s saved custom input binding (empty if none saved yet).
std::wstring Settings::LoadLaneBinding(int lane)
{
    wchar_t buffer[MAX_PATH] = L"";
    std::wstring key = L"LaneBinding" + std::to_wstring(lane);
    GetPrivateProfileStringW(L"App", key.c_str(), L"", buffer, MAX_PATH, GetSettingsFilePath().c_str());
    return buffer;
}

// Saves lane index `lane`'s custom input binding so it's restored next launch.
void Settings::SaveLaneBinding(int lane, const std::wstring& serialized)
{
    std::wstring key = L"LaneBinding" + std::to_wstring(lane);
    WritePrivateProfileStringW(L"App", key.c_str(), serialized.c_str(), GetSettingsFilePath().c_str());
}
