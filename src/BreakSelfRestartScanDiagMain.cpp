#include <cstdio>
#include <unordered_map>

#include "ChartFile.h"

// Standalone diagnostic (not part of the normal build): investigates a
// reported bug - "sometimes, when starting a Break section, the previous
// wav file(s) will loop and play a tiny bit of the beginning of their
// track." GameSession::BeginSection's Break case calls
// m_audioEngine.StopAll() and THEN, moments later in the same synchronous
// call, StartClipLoop(section.clipIndex, ...) for the break's own clip. If
// that clip is the SAME one that was already playing (background/learn,
// uninterrupted) right up until this exact Break section began,
// StopAll()'s Stop()+FlushSourceBuffers() on that voice is immediately
// followed by a fresh SubmitSourceBuffer+Start() on the SAME voice,
// phase-seeked to 0 (ForgetStaleClipOrigin re-anchors it, since isPlaying
// was just cleared) - i.e. the clip's own true beginning. XAudio2's
// Stop()/FlushSourceBuffers() only take effect at the start of the next
// ~10ms processing quantum (not synchronously) - so a few ms of the OLD
// voice's audio can still play out after Stop() is called, immediately
// followed by the NEW voice's audio starting at sample 0 - which would
// sound exactly like "a tiny bit of the beginning of their track" bleeding
// in right as the break's own clip starts. This can ONLY happen for a
// clip that's genuinely STILL PLAYING right up to the exact instant of its
// own Break section (not one silenced earlier by an intervening Reset/
// Break) - an earlier version of ClipReuseAfterStopScanDiagMain.cpp
// couldn't distinguish this case from an ordinary reuse-after-stop, since
// it read isPlaying AFTER already applying the current section's own
// StopAll(). This scanner reads it before, specifically to find real
// occurrences of this exact shape across every chart in the repo.

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

    std::unordered_map<const ChartClip*, bool> isPlaying;
    bool anyFlagged = false;

    for (const ChartSection& section : song.sections)
    {
        if (section.kind == SectionKind::Break && section.clipIndex >= 0)
        {
            const ChartClip* clip = &song.clips[section.clipIndex];
            // Read BEFORE this section's own StopAll() below - the whole
            // point is to catch "was this the break's own clip, already
            // playing right up to this exact instant".
            if (isPlaying[clip])
            {
                anyFlagged = true;
                wprintf(L"  FLAGGED: [break] clip '%ls' was still playing (uninterrupted) right up to this exact "
                        L"break section - StopAll() immediately followed by StartClipLoop restarting the SAME "
                        L"voice from its own beginning.\n",
                        clip->name.c_str());
            }
        }

        if (section.kind == SectionKind::Reset || section.kind == SectionKind::Break)
        {
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
            isPlaying[clip] = true;
        }
    }

    if (!anyFlagged)
    {
        wprintf(L"  (no break-restarts-its-own-still-playing-clip pattern found)\n");
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
