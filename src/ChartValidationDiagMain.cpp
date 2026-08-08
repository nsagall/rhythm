#include <cstdio>
#include <string>
#include <vector>

#include "ChartFile.h"

// Standalone diagnostic (not part of the normal build): runs ChartFile::Load
// against a deliberately broken chart (to confirm every kind of validation
// error is caught and reported), against a handful of charts each
// deliberately broken in one midi_file-specific way, and against every real
// chart in the repo (to confirm none of them regress to reporting spurious
// errors).

namespace
{

void RunOne(const std::wstring& path, bool expectSuccess)
{
    ChartSong song;
    std::vector<std::wstring> errors;
    bool ok = ChartFile::Load(path, song, errors);

    wprintf(L"=== %ls ===\n", path.c_str());
    wprintf(L"  Load() returned %ls (expected %ls)\n", ok ? L"true" : L"false", expectSuccess ? L"true" : L"false");
    if (ok != expectSuccess)
    {
        wprintf(L"  ** MISMATCH **\n");
    }
    for (const std::wstring& error : errors)
    {
        wprintf(L"  - %ls\n", error.c_str());
    }
    wprintf(L"\n");
}

} // namespace

int main()
{
    RunOne(L"test_charts/broken_duplicate_clip.chart", false);
    RunOne(L"test_charts/broken_unknown_clip_section.chart", false);
    RunOne(L"test_charts/broken_invalid_play_mode.chart", false);
    RunOne(L"test_charts/broken_empty_clip_learn.chart", false);
    RunOne(L"test_charts/broken_empty_clip_background.chart", false);
    RunOne(L"test_charts/broken_zero_sections.chart", false);
    RunOne(L"test_charts/broken_learn_no_midi.chart", false);

    RunOne(L"test_charts/test_song.chart", true);
    RunOne(L"test_charts/tile_test.chart", true);
    RunOne(L"test_charts/fast_test.chart", true);
    RunOne(L"test_charts/volume_test.chart", true);
    RunOne(L"test_charts/multilane_test.chart", true);
    RunOne(L"test_charts/section_modes_test.chart", true);
    RunOne(L"test_charts/no_midi_solo_test.chart", true);
    RunOne(L"test_charts/solo_ending_test.chart", true);
    RunOne(L"Content/Cool Boy/Cool Boy.chart", true);
    RunOne(L"Content/Melodius/Melodius.chart", true);
    RunOne(L"Content/A Real Good Time/A Real Good Time.chart", true);
    RunOne(L"Content/Better/better.chart", true);
    RunOne(L"Content/Byte Blaster (AI Slop)/Byte Blaster.chart", true);
    RunOne(L"Content/Forest/forest.chart", true);
    RunOne(L"Content/Voltage Run (AI Slop)/Voltage Run.chart", true);

    RunOne(L"test_charts/does_not_exist.chart", false);

    return 0;
}
