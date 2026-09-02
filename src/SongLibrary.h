#pragma once

#include <string>
#include <vector>

// One playable song found under the content root: a subdirectory holding a single loadable .chart file.
struct SongEntry
{
    std::wstring title;     // ChartSong::title, or the folder name if the chart declares none.
    std::wstring chartPath; // Full path to the .chart file.
};

// Scrapes a content folder for playable songs.
class SongLibrary
{
public:
    // Scans every immediate subdirectory of contentRoot for a .chart file that parses.
    //   contentRoot         - the content folder to scan.
    //   outValidationErrors - if non-null, one message (chart path + every ChartSong::Load error)
    //                         appended per subdirectory whose .chart fails to parse.
    // Returns one SongEntry per subdirectory with a valid .chart, sorted by title. A subdirectory
    // with no .chart is silently skipped; one with more than one uses the first found. Returns an
    // empty list (not an error) if contentRoot doesn't exist.
    static std::vector<SongEntry> Scrape(const std::wstring& contentRoot,
                                          std::vector<std::wstring>* outValidationErrors = nullptr);
};
