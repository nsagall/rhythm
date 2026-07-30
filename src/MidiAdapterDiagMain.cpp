#include <cstdio>
#include <string>
#include <vector>

#include "ChartMidi.h"

// Standalone diagnostic (not part of the normal build): runs
// ChartMidi::LoadLaneNotes against the project's test .mid fixtures and
// prints every extracted note per lane plus totalBeats, so the pitch
// filtering/tick-to-beat conversion/per-lane bucketing can be checked
// without going through a whole chart or the running app.

namespace
{

void RunOne(const std::wstring& path)
{
    MidiLaneData data;
    std::wstring error;
    bool ok = ChartMidi::LoadLaneNotes(path, data, error);

    wprintf(L"=== %ls ===\n", path.c_str());
    if (!ok)
    {
        wprintf(L"  FAILED: %ls\n\n", error.c_str());
        return;
    }

    wprintf(L"  totalBeats = %.3f\n", data.totalBeats);
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        wprintf(L"  lane %d (%zu notes):\n", lane, data.lanes[lane].size());
        for (const LaneNote& note : data.lanes[lane])
        {
            wprintf(L"    start=%.3f duration=%.3f\n", note.startBeat, note.durationBeats);
        }
    }
    wprintf(L"\n");
}

} // namespace

int main()
{
    RunOne(L"test_charts/fast_test_hat.mid");
    RunOne(L"test_charts/multilane_test.mid");
    RunOne(L"Content/Cool Boy/Kick Snare.mid");
    RunOne(L"Content/Cool Boy/HH.mid");
    RunOne(L"Content/Cool Boy/Bass.mid");
    RunOne(L"Content/Cool Boy/Phee.mid");

    // Deliberately broken fixtures.
    RunOne(L"test_charts/dangling_note.mid");
    RunOne(L"test_charts/malformed.mid");
    RunOne(L"test_charts/does_not_exist.mid");
    RunOne(L"C:\\Users\\nsaga\\AppData\\Local\\Temp\\claude\\C--Users-nsaga-OneDrive-Projects\\2d6d0060-ad5a-4bf4-86f9-077b215135dd\\scratchpad\\overlapping_notes.mid");
    RunOne(L"C:\\Users\\nsaga\\AppData\\Local\\Temp\\claude\\C--Users-nsaga-OneDrive-Projects\\2d6d0060-ad5a-4bf4-86f9-077b215135dd\\scratchpad\\zero_length_note.mid");

    return 0;
}
