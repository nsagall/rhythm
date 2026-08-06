#include <cstdio>

#include "Settings.h"

// Standalone diagnostic (not part of the normal build): verifies Settings'
// high score persistence (LoadHighScores/SaveHighScores/HighScoreQualifies/
// InsertHighScore) in isolation, with no chart or GameSession involved.
// Uses a dedicated songKey unlikely to collide with a real song's content
// folder name, and leaves that key erased (an empty saved list) when it's
// done either way, so a normal run never leaves test data behind in the
// real %APPDATA%\Rhythm\settings.ini.

namespace
{
const std::wstring kTestSongKey = L"__ScoringDiagTest__";
bool g_anyFailure = false;

void Check(bool condition, const char* description)
{
    printf("%s: %s\n", condition ? "OK  " : "FAIL", description);
    if (!condition)
    {
        g_anyFailure = true;
    }
}
} // namespace

int main()
{
    Settings settings;

    // Start from a clean slate, in case a previous run crashed before its
    // own cleanup at the bottom ran.
    settings.SaveHighScores(kTestSongKey, {});

    Check(settings.LoadHighScores(kTestSongKey).empty(), "fresh key loads as empty");
    Check(Settings::HighScoreQualifies({}, 1), "any score qualifies for an empty list");

    // Fill to exactly kMaxHighScoreEntries, deliberately out of order, and
    // confirm InsertHighScore leaves it sorted descending by score.
    std::vector<HighScoreEntry> entries;
    for (int i = 0; i < Settings::kMaxHighScoreEntries; ++i)
    {
        // Scores 100, 200, ..., 1000, inserted in a scrambled order (evens
        // then odds) so a correct sort can't happen to fall out of
        // insertion order by accident.
        int rank = (i % 2 == 0) ? i / 2 : Settings::kMaxHighScoreEntries - 1 - i / 2;
        int score = (rank + 1) * 100;
        Settings::InsertHighScore(entries, L"AAA", score);
    }
    Check(entries.size() == static_cast<size_t>(Settings::kMaxHighScoreEntries), "list trimmed to kMaxHighScoreEntries");
    bool sortedDescending = true;
    for (size_t i = 1; i < entries.size(); ++i)
    {
        if (entries[i - 1].score < entries[i].score)
        {
            sortedDescending = false;
        }
    }
    Check(sortedDescending, "InsertHighScore leaves the list sorted highest-first");
    Check(entries.front().score == 1000, "highest score (1000) landed at rank 0");
    Check(entries.back().score == 100, "lowest score (100) landed at the last rank");

    // A full list: only a strictly-better-than-worst score still qualifies.
    Check(!Settings::HighScoreQualifies(entries, 50), "a low score no longer qualifies once the list is full");
    Check(!Settings::HighScoreQualifies(entries, 100), "a tying score doesn't bump the existing lowest entry");
    Check(Settings::HighScoreQualifies(entries, 150), "a score beating the current lowest still qualifies");

    // Inserting a genuinely new best pushes the old lowest entry out.
    Settings::InsertHighScore(entries, L"ZZZ", 5000);
    Check(entries.size() == static_cast<size_t>(Settings::kMaxHighScoreEntries),
          "inserting into a full list still trims back to kMaxHighScoreEntries");
    Check(entries.front().initials == L"ZZZ" && entries.front().score == 5000,
          "the new best score is now rank 0");
    bool oldLowestStillPresent = false;
    for (const HighScoreEntry& entry : entries)
    {
        if (entry.score == 100)
        {
            oldLowestStillPresent = true;
        }
    }
    Check(!oldLowestStillPresent, "the old lowest entry (100) was pushed out of the top 10");

    // Round-trips through the actual INI-backed storage.
    settings.SaveHighScores(kTestSongKey, entries);
    std::vector<HighScoreEntry> reloaded = settings.LoadHighScores(kTestSongKey);
    Check(reloaded.size() == entries.size(), "reloaded list has the same size as what was saved");
    bool roundTripMatches = reloaded.size() == entries.size();
    for (size_t i = 0; roundTripMatches && i < entries.size(); ++i)
    {
        if (reloaded[i].score != entries[i].score || reloaded[i].initials != entries[i].initials)
        {
            roundTripMatches = false;
        }
    }
    Check(roundTripMatches, "every reloaded entry (score + initials) matches what was saved, in the same order");

    // A shorter re-save must erase the now-stale trailing ranks, not leave
    // them behind from the longer list above.
    std::vector<HighScoreEntry> shortList = {{L"ABC", 999}};
    settings.SaveHighScores(kTestSongKey, shortList);
    std::vector<HighScoreEntry> reloadedShort = settings.LoadHighScores(kTestSongKey);
    Check(reloadedShort.size() == 1, "saving a shorter list erases the previous list's stale trailing ranks");

    // Cleanup: leave no test data behind in the real settings file.
    settings.SaveHighScores(kTestSongKey, {});
    Check(settings.LoadHighScores(kTestSongKey).empty(), "cleanup: test key is empty again");

    printf("\n%s\n", g_anyFailure ? "*** ONE OR MORE CHECKS FAILED ***" : "All checks passed.");
    return g_anyFailure ? 1 : 0;
}
