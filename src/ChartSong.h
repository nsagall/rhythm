#pragma once

#include <string>
#include <vector>

#include "ChartClip.h"

// Which of the four block kinds a section is.
enum class SectionKind
{
    Learn,      // [learn]: judge presses/releases against the clip, exactly like today's single-clip flow
    Break,      // [break]: stop everything else playing, play this clip, block until loop_count loops finish
    Reset,      // [reset]: stop everything else playing, then advance immediately (a silence gate, no clip)
    Background, // [background]: queue this clip to start playing (without stopping anything) when the *next* section begins
};

// One step of actual gameplay, processed in declared order - clips alone
// do nothing; only sections drive the song. clipIndex is resolved at
// parse time to an index into ChartSong::clips, or -1 for a Reset section
// (the only kind with no clip).
struct ChartSection
{
    int clipIndex = -1;
    SectionKind kind = SectionKind::Learn;
    int loopCount = 1; // minimum number of times the clip must loop; see SectionKind-specific semantics in GameSession
};

// A full song: tempo/time signature, the pool of reusable clips, and the
// ordered list of sections that actually drives gameplay.
class ChartSong
{
public:
    std::wstring title;
    double bpm = 120.0;
    int beatsPerBar = 4; // the time_signature field's numerator (N in "N/D")
    // The time_signature field's denominator (D in "N/D") - retained purely
    // so a caller that needs to redisplay/re-save the chart's own declared
    // time signature (see EditorChartIO) doesn't need to re-derive or
    // re-parse it separately; nothing in judging/timing ever reads this,
    // only beatsPerBar does.
    int timeSignatureDenominator = 4;
    // Default press/release judging tolerance for any clip that doesn't
    // declare its own start_tolerance_ms/release_tolerance_ms override.
    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;
    std::vector<ChartClip> clips;
    std::vector<ChartSection> sections;

    // Parses and validates a .chart text file into *this - the only way a
    // chart ever becomes a ChartSong; nothing else in this codebase
    // constructs one by hand. Returns false if the file can't be opened or
    // fails validation - outErrors then holds a human-readable message for
    // every problem found (unsupported fields, wrong-typed or out-of-range
    // values, a malformed time signature, missing required fields, a
    // referenced stem/MIDI file that doesn't exist or can't be parsed, a
    // duplicate clip name, a section referencing an unknown clip, an
    // unrecognized block header, or a chart with no
    // [learn]/[break]/[reset]/[background] blocks at all). On failure *this
    // is left exactly as it was before the call - a partially-parsed chart
    // is never left half-applied. On success outErrors is empty and *this
    // holds the fully parsed/validated song.
    bool Load(const std::wstring& chartFilePath, std::vector<std::wstring>& outErrors);
};
