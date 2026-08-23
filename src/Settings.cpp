#include "Settings.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

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

// Turns songKey into a string safe to embed in an INI key name - every
// character that isn't alphanumeric (spaces, punctuation a song's folder
// name might contain) becomes '_'. Two different song folder names that
// differ only in punctuation could in principle collide after this, but
// that's not a real concern for a personal song library.
std::wstring SanitizeForIniKey(const std::wstring& raw)
{
    std::wstring out = raw;
    for (wchar_t& c : out)
    {
        if (!std::iswalnum(static_cast<wint_t>(c)))
        {
            c = L'_';
        }
    }
    return out;
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

// Reads songKey's saved high scores (see the header's own comment).
std::vector<HighScoreEntry> Settings::LoadHighScores(const std::wstring& songKey)
{
    std::wstring path = GetSettingsFilePath();
    std::wstring keyBase = SanitizeForIniKey(songKey);

    std::vector<HighScoreEntry> entries;
    for (int rank = 0; rank < c_MaxHighScoreEntries; ++rank)
    {
        std::wstring scoreKey = keyBase + L"_Score" + std::to_wstring(rank);
        // -1 as the missing-key default (scores are never negative) doubles
        // as "no more ranks saved" - SaveHighScores always writes ranks
        // contiguously from 0, so the first missing one ends the list.
        int score = GetPrivateProfileIntW(L"HighScores", scoreKey.c_str(), -1, path.c_str());
        if (score < 0)
        {
            break;
        }

        std::wstring initialsKey = keyBase + L"_Initials" + std::to_wstring(rank);
        wchar_t buffer[8] = L"";
        GetPrivateProfileStringW(L"HighScores", initialsKey.c_str(), L"", buffer, 8, path.c_str());
        entries.push_back({buffer, score});
    }
    return entries;
}

// Saves songKey's high scores (see the header's own comment).
void Settings::SaveHighScores(const std::wstring& songKey, const std::vector<HighScoreEntry>& entries)
{
    std::wstring path = GetSettingsFilePath();
    std::wstring keyBase = SanitizeForIniKey(songKey);

    for (int rank = 0; rank < c_MaxHighScoreEntries; ++rank)
    {
        std::wstring scoreKey = keyBase + L"_Score" + std::to_wstring(rank);
        std::wstring initialsKey = keyBase + L"_Initials" + std::to_wstring(rank);
        if (rank < static_cast<int>(entries.size()))
        {
            WritePrivateProfileStringW(L"HighScores", scoreKey.c_str(),
                                        std::to_wstring(entries[rank].score).c_str(), path.c_str());
            WritePrivateProfileStringW(L"HighScores", initialsKey.c_str(), entries[rank].initials.c_str(),
                                        path.c_str());
        }
        else
        {
            // A previously-longer list left stale entries at this rank - erase them.
            WritePrivateProfileStringW(L"HighScores", scoreKey.c_str(), nullptr, path.c_str());
            WritePrivateProfileStringW(L"HighScores", initialsKey.c_str(), nullptr, path.c_str());
        }
    }
}

// See the header's own comment.
bool Settings::HighScoreQualifies(const std::vector<HighScoreEntry>& entries, int score)
{
    if (static_cast<int>(entries.size()) < c_MaxHighScoreEntries)
    {
        return true;
    }
    return score > entries.back().score;
}

// See the header's own comment.
void Settings::InsertHighScore(std::vector<HighScoreEntry>& entries, const std::wstring& initials, int score)
{
    entries.push_back({initials, score});
    std::sort(entries.begin(), entries.end(),
              [](const HighScoreEntry& a, const HighScoreEntry& b) { return a.score > b.score; });
    if (entries.size() > static_cast<size_t>(c_MaxHighScoreEntries))
    {
        entries.resize(c_MaxHighScoreEntries);
    }
}
