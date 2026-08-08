#include <cstdio>
#include <unordered_map>

#include "ChartFile.h"

// Standalone diagnostic (not part of the normal build): a lightweight,
// audio-free structural scanner (no AudioEngine, no GameSession, no real
// timing) that flags every clip reuse pattern that was exposed to the
// stale-clip-origin bug GameSession::ForgetStaleClipOrigin/
// BlockSchedule.cpp's own forgetStaleOrigin fixed: a clip used by a
// [background]/[learn]/[break] section, then STOPPED by a later
// [reset]/[break]'s StopAll(), then reused again by a still-later section -
// exactly the shape that made "A Real Good Time"'s [learn] clip=bass
// present its pattern's notes out of order (the true first note pushed to
// the very end) before the fix. Doesn't replay real gameplay (no
// hits_required/tolerance timing) - purely a static "which charts have this
// shape at all" survey, run across every real chart in Content/, so a
// human/agent can decide whether any of them warrant the same kind of full
// GameSession replay verification StaleClipOriginDiagMain.cpp did for bass.

namespace
{

void ScanChart(const std::wstring& path)
{
    ChartSong song;
    std::vector<std::wstring> errors;
    if (!ChartFile::Load(path, song, errors))
    {
        wprintf(L"=== %ls === FAILED TO LOAD\n", path.c_str());
        return;
    }

    wprintf(L"=== %ls ===\n", path.c_str());

    std::unordered_map<const ChartClip*, bool> everUsed;
    std::unordered_map<const ChartClip*, bool> isPlaying;
    bool anyFlagged = false;

    for (const ChartSection& section : song.sections)
    {
        if (section.kind == SectionKind::Reset || section.kind == SectionKind::Break)
        {
            // Mirrors GameSession::BeginSection's Reset/Break StopAll() -
            // silences every currently-open clip, regardless of how it
            // started.
            for (auto& entry : isPlaying)
            {
                entry.second = false;
            }
        }
        if (section.clipIndex < 0)
        {
            continue;
        }
        const ChartClip* clip = &song.clips[section.clipIndex];

        if (section.kind == SectionKind::Learn || section.kind == SectionKind::Background ||
            section.kind == SectionKind::Break)
        {
            bool wasEverUsed = everUsed[clip];
            bool wasPlaying = isPlaying[clip];
            if (wasEverUsed && !wasPlaying)
            {
                anyFlagged = true;
                wprintf(L"  FLAGGED: clip '%ls' reused by a [%ls] section after having been stopped since its "
                        L"earlier use - this is the shape ForgetStaleClipOrigin/forgetStaleOrigin fixed.\n",
                        clip->name.c_str(),
                        section.kind == SectionKind::Learn ? L"learn"
                        : section.kind == SectionKind::Background ? L"background"
                                                                   : L"break");
            }
            else if (wasEverUsed && wasPlaying)
            {
                // The OTHER case ForgetStaleClipOrigin must NOT touch - a
                // clip still genuinely playing (never stopped since its
                // earlier use) must keep its established groove. Purely
                // informational (not itself a bug signal) - flags real
                // charts worth a live-GameSession spot check for this case.
                wprintf(L"  (still-playing reuse: clip '%ls' reused by a [%ls] section while never having been "
                        L"stopped since its earlier use - origin must be preserved, not reset)\n",
                        clip->name.c_str(),
                        section.kind == SectionKind::Learn ? L"learn"
                        : section.kind == SectionKind::Background ? L"background"
                                                                   : L"break");
            }
            everUsed[clip] = true;
            isPlaying[clip] = true;
        }
    }

    if (!anyFlagged)
    {
        wprintf(L"  (no reuse-after-stop pattern found)\n");
    }
    wprintf(L"\n");
}

} // namespace

int main()
{
    ScanChart(L"Content/A Real Good Time/A Real Good Time.chart");
    ScanChart(L"Content/Cool Boy/Cool Boy.chart");
    ScanChart(L"Content/Melodius/Melodius.chart");
    ScanChart(L"Content/Better/better.chart");
    ScanChart(L"Content/Byte Blaster (AI Slop)/Byte Blaster.chart");
    ScanChart(L"Content/Forest/forest.chart");
    ScanChart(L"Content/Voltage Run (AI Slop)/Voltage Run.chart");
    return 0;
}
