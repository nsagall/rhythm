#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic: loads real Content/ charts twice each (easyMode false and true) and, for
// every judged clip, prints each lane's note count and a compact {beat:lane} list before vs. after
// ApplyEasyModeTransform. Lets the density-thinning rules be eyeballed against real content: a
// dense clip should thin while keeping irregular gaps, an already-sparse clip should be identical.

namespace
{

// One clip's {beat,lane} pairs across all lanes, merged and sorted by beat.
struct FlatNote
{
    double beat;
    int lane;
    double duration;
};

std::vector<FlatNote> Flatten(const ChartClip& clip)
{
    std::vector<FlatNote> notes;
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        for (const LaneNote& note : clip.LaneNotes(lane))
        {
            notes.push_back({note.startBeat, lane, note.durationBeats});
        }
    }
    std::sort(notes.begin(), notes.end(), [](const FlatNote& a, const FlatNote& b)
              {
                  if (a.beat != b.beat)
                  {
                      return a.beat < b.beat;
                  }
                  return a.lane < b.lane;
              });
    return notes;
}

void PrintFlat(const std::vector<FlatNote>& notes)
{
    printf("    ");
    for (size_t i = 0; i < notes.size(); ++i)
    {
        printf("{%.2f,L%d,d%.2f}%s", notes[i].beat, notes[i].lane, notes[i].duration,
               (i + 1 < notes.size()) ? " " : "");
    }
    printf("\n");
}

void DumpChart(const std::wstring& chartPath)
{
    AudioEngine engine;
    if (!engine.Initialize())
    {
        wprintf(L"AudioEngine::Initialize failed for %ls\n", chartPath.c_str());
        return;
    }

    GameSession hard(engine);
    GameSession easy(engine);
    std::wstring error;
    bool hardOk = hard.LoadChart(chartPath, /*easyMode=*/false, error);
    if (!hardOk)
    {
        wprintf(L"hard-mode LoadChart failed for %ls: %ls\n", chartPath.c_str(), error.c_str());
        engine.Shutdown();
        return;
    }
    bool easyOk = easy.LoadChart(chartPath, /*easyMode=*/true, error);
    if (!easyOk)
    {
        wprintf(L"easy-mode LoadChart failed for %ls: %ls\n", chartPath.c_str(), error.c_str());
        engine.Shutdown();
        return;
    }

    wprintf(L"=== %ls (bpm=%.2f) ===\n", chartPath.c_str(), hard.Song().Bpm());
    const ChartSong& hardSong = hard.Song();
    const ChartSong& easySong = easy.Song();
    for (size_t i = 0; i < hardSong.Clips().size(); ++i)
    {
        const ChartClip& hardClip = hardSong.Clips()[i];
        const ChartClip& easyClip = easySong.Clips()[i];
        if (!hardClip.HasMidi())
        {
            continue;
        }
        std::vector<FlatNote> before = Flatten(hardClip);
        std::vector<FlatNote> after = Flatten(easyClip);
        int totalBefore = static_cast<int>(before.size());
        int totalAfter = static_cast<int>(after.size());
        wprintf(L"  clip '%ls' span=%.2f: %d notes -> %d notes%ls\n", hardClip.Name().c_str(), hardClip.SpanBeats(),
                totalBefore, totalAfter, (totalBefore == totalAfter) ? L"  (unchanged - already sparse enough)" : L"");
        printf("  before:\n");
        PrintFlat(before);
        printf("  after:\n");
        PrintFlat(after);
    }
    printf("\n");

    engine.Shutdown();
}

} // namespace

int main()
{
    DumpChart(L"Content/Cool Boy/Cool Boy.chart");
    DumpChart(L"Content/Byte Blaster (AI Slop)/Byte Blaster.chart");
    DumpChart(L"Content/Better/better.chart");
    DumpChart(L"Content/A Real Good Time/A Real Good Time.chart");
    return 0;
}
