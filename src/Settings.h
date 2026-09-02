#pragma once

#include <string>
#include <vector>

// One entry in a song's top-10 high score list.
struct HighScoreEntry
{
    // Exactly 3 letters, e.g. L"ABC".
    std::wstring initials;

    int score = 0;
};

// Persists app-level preferences between runs, backed by an INI file under %APPDATA%\Rhythm.
class Settings
{
public:
    // How many entries a song's high score list holds at most.
    static constexpr int c_MaxHighScoreEntries = 10;

    // Reads the saved last-chart path from disk (empty if none saved yet).
    std::wstring LoadLastChartPath();

    // Saves the given chart path so it's pre-filled next launch.
    void SaveLastChartPath(const std::wstring& chartPath);

    // Reads the saved Easy Mode toggle state from disk (false if none saved yet).
    bool LoadEasyMode();

    // Saves the Easy Mode toggle state so it's restored next launch.
    void SaveEasyMode(bool easyMode);

    // Reads lane `lane`'s saved custom input binding, serialized by LaneBindings. Settings doesn't
    // interpret the string, only round-trips it. Empty if none saved.
    std::wstring LoadLaneBinding(int lane);

    // Saves lane `lane`'s custom input binding, already serialized by LaneBindings.
    void SaveLaneBinding(int lane, const std::wstring& serialized);

    // Reads songKey's saved high scores, highest first, at most c_MaxHighScoreEntries; empty if
    // none saved. songKey is sanitized internally before becoming an INI key, so any characters
    // are safe to pass.
    std::vector<HighScoreEntry> LoadHighScores(const std::wstring& songKey);

    // Saves songKey's high scores. entries must already be sorted highest-first and trimmed to
    // c_MaxHighScoreEntries (see InsertHighScore); this doesn't re-sort or re-trim. Ranks beyond
    // entries.size() from a previously-longer list are erased.
    void SaveHighScores(const std::wstring& songKey, const std::vector<HighScoreEntry>& entries);

    // True if score would earn a spot in entries - either the list isn't full, or score beats its
    // lowest entry.
    static bool HighScoreQualifies(const std::vector<HighScoreEntry>& entries, int score);

    // Inserts {initials, score} into entries in descending-score order and trims to
    // c_MaxHighScoreEntries, ready for SaveHighScores.
    static void InsertHighScore(std::vector<HighScoreEntry>& entries, const std::wstring& initials, int score);
};
