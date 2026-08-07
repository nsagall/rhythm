#pragma once

#include <string>
#include <vector>

// One entry in a song's top-10 high score list - see
// Settings::LoadHighScores/SaveHighScores.
struct HighScoreEntry
{
    std::wstring initials; // exactly 3 letters, e.g. L"ABC"
    int score = 0;
};

// Persists app-level preferences between runs, backed by an INI file under %APPDATA%\Rhythm.
class Settings
{
public:
    // How many entries a song's high score list holds at most - LoadHighScores
    // never returns more than this, and InsertHighScore trims back down to it.
    static constexpr int kMaxHighScoreEntries = 10;

    // Reads the saved last-chart path from disk (empty if none saved yet).
    std::wstring LoadLastChartPath();

    // Saves the given chart path so it's pre-filled next launch.
    void SaveLastChartPath(const std::wstring& chartPath);

    // Reads the saved Easy Mode toggle state from disk (false if none saved yet).
    bool LoadEasyMode();

    // Saves the Easy Mode toggle state so it's restored next launch.
    void SaveEasyMode(bool easyMode);

    // Reads lane index `lane`'s saved custom input binding, serialized by
    // LaneBindings (see src/LaneBindings.h) - Settings itself doesn't know or
    // care what the string means, only that it round-trips. Empty if none
    // saved yet.
    std::wstring LoadLaneBinding(int lane);

    // Saves lane index `lane`'s custom input binding, already serialized by
    // LaneBindings, so it's restored next launch.
    void SaveLaneBinding(int lane, const std::wstring& serialized);

    // Reads songKey's saved high scores, highest score first, at most
    // kMaxHighScoreEntries - empty if none saved yet. songKey identifies a
    // song (MainWindow derives it from the song's content folder name);
    // sanitized internally before becoming part of an INI key, so any
    // folder-name characters are safe to pass in as-is.
    std::vector<HighScoreEntry> LoadHighScores(const std::wstring& songKey);

    // Saves songKey's high scores. entries must already be sorted
    // highest-first and trimmed to at most kMaxHighScoreEntries (see
    // InsertHighScore) - this call doesn't re-sort or re-trim, and any ranks
    // beyond entries.size() left over from a previously-longer list are
    // erased.
    void SaveHighScores(const std::wstring& songKey, const std::vector<HighScoreEntry>& entries);

    // True if score would earn a spot in entries (a song's current high
    // score list, as returned by LoadHighScores) - either the list isn't
    // full yet, or score beats its current lowest entry.
    static bool HighScoreQualifies(const std::vector<HighScoreEntry>& entries, int score);

    // Inserts {initials, score} into entries in descending-score order and
    // trims back down to kMaxHighScoreEntries - entries is left ready to
    // hand straight to SaveHighScores.
    static void InsertHighScore(std::vector<HighScoreEntry>& entries, const std::wstring& initials, int score);
};
