#include "SongLibrary.h"

#include <algorithm>
#include <filesystem>

#include "ChartFile.h"

namespace fs = std::filesystem;

std::vector<SongEntry> SongLibrary::Scrape(const std::wstring& contentRoot)
{
    std::vector<SongEntry> songs;

    std::error_code dirEc;
    if (!fs::is_directory(contentRoot, dirEc))
    {
        return songs;
    }

    for (const fs::directory_entry& folder : fs::directory_iterator(contentRoot, dirEc))
    {
        if (dirEc || !folder.is_directory())
        {
            continue;
        }

        std::wstring chartPath;
        std::error_code fileEc;
        for (const fs::directory_entry& file : fs::directory_iterator(folder.path(), fileEc))
        {
            if (!fileEc && file.is_regular_file() && file.path().extension() == L".chart")
            {
                chartPath = file.path().wstring();
                break;
            }
        }
        if (chartPath.empty())
        {
            continue;
        }

        ChartSong song;
        std::vector<std::wstring> errors;
        if (!ChartFile::Load(chartPath, song, errors))
        {
            continue;
        }

        std::wstring title = song.title.empty() ? folder.path().filename().wstring() : song.title;
        songs.push_back({title, chartPath});
    }

    std::sort(songs.begin(), songs.end(),
              [](const SongEntry& a, const SongEntry& b) { return a.title < b.title; });
    return songs;
}
