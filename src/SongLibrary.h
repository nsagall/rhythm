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

class SongLibrary
{
public:
    // Scans every immediate subdirectory of contentRoot for a .chart file
    // that parses successfully, and returns one SongEntry per subdirectory
    // where one was found - sorted by title. A subdirectory with no .chart
    // file, or whose .chart fails to parse, is silently skipped: this is a
    // casual library listing, not a validation report (that's what
    // ChartValidationDiagMain is for). If a subdirectory somehow has more
    // than one .chart file, only the first one found is used. Returns an
    // empty list (not an error) if contentRoot itself doesn't exist.
    static std::vector<SongEntry> Scrape(const std::wstring& contentRoot);
};
