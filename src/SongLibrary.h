#pragma once

#include <string>
#include <vector>

// One playable song found under the content root: a subdirectory holding a
// single loadable .chart file.
struct SongEntry
{
    std::wstring title;     // ChartSong::title, or the folder name if the chart declares none
    std::wstring chartPath; // full path to the .chart file
};

// Scrapes a content folder for playable songs - the only way MainWindow's
// song-select list gets populated.
class SongLibrary
{
public:
    // Scans every immediate subdirectory of contentRoot for a .chart file
    // that parses successfully, and returns one SongEntry per subdirectory
    // where one was found - sorted by title. A subdirectory with no .chart
    // file is silently skipped either way - there's nothing to validate.
    // A subdirectory whose .chart file fails to parse is always skipped
    // from the returned list too, but if outValidationErrors is non-null,
    // one descriptive message (chart path + every error ChartSong::Load
    // reported) is appended to it per such failure, so a caller that wants
    // to surface these (e.g. an explicit user-initiated rescan) can, while
    // the default (nullptr) stays a casual, silent-skip library listing.
    // If a subdirectory somehow has more than one .chart file, only the
    // first one found is used. Returns an empty list (not an error) if
    // contentRoot itself doesn't exist.
    static std::vector<SongEntry> Scrape(const std::wstring& contentRoot,
                                          std::vector<std::wstring>* outValidationErrors = nullptr);
};
