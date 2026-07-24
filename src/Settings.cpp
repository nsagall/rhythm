#include "Settings.h"

#include <windows.h>

#include <string>

namespace {

std::wstring GetSettingsFilePath() {
    wchar_t appDataDir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appDataDir, MAX_PATH);

    std::wstring dir = std::wstring(appDataDir) + L"\\Rhythm";
    CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\settings.ini";
}

} // namespace

std::array<MusicTrack, kTrackCount> Settings::Load() {
    std::array<MusicTrack, kTrackCount> tracks;
    std::wstring iniPath = GetSettingsFilePath();

    for (int i = 0; i < kTrackCount; ++i) {
        wchar_t fileKey[16];
        wsprintfW(fileKey, L"File%d", i);
        wchar_t countKey[16];
        wsprintfW(countKey, L"Count%d", i);
        wchar_t bpmKey[16];
        wsprintfW(bpmKey, L"Bpm%d", i);

        wchar_t filePath[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"Tracks", fileKey, L"", filePath, MAX_PATH, iniPath.c_str());

        int repeatCount = GetPrivateProfileIntW(L"Tracks", countKey, 1, iniPath.c_str());
        int bpm = GetPrivateProfileIntW(L"Tracks", bpmKey, 120, iniPath.c_str());

        tracks[i].filePath = filePath;
        tracks[i].repeatCount = repeatCount < 1 ? 1 : repeatCount;
        tracks[i].bpm = bpm < 1 ? 1 : bpm;
    }

    return tracks;
}

void Settings::Save(const std::array<MusicTrack, kTrackCount>& tracks) {
    std::wstring iniPath = GetSettingsFilePath();

    for (int i = 0; i < kTrackCount; ++i) {
        wchar_t fileKey[16];
        wsprintfW(fileKey, L"File%d", i);
        wchar_t countKey[16];
        wsprintfW(countKey, L"Count%d", i);
        wchar_t bpmKey[16];
        wsprintfW(bpmKey, L"Bpm%d", i);

        WritePrivateProfileStringW(L"Tracks", fileKey, tracks[i].filePath.c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Tracks", countKey, std::to_wstring(tracks[i].repeatCount).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Tracks", bpmKey, std::to_wstring(tracks[i].bpm).c_str(), iniPath.c_str());
    }
}
