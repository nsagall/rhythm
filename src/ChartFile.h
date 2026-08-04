#pragma once

#include <string>
#include <vector>

#include "ChartMidi.h"
#include "LaneConfig.h"

// A reusable audio+MIDI bundle: a stem file, its MIDI-authored note
// pattern, and the thresholds needed to judge/lock it in when played in a
// [learn] section. A clip is purely a definition - it does nothing on its
// own until a section block references it, and the same clip may be
// referenced by more than one section (e.g. once as a background layer,
// later as the thing being learned). laneNotes[i] holds lane i's own
// independent, 0-indexed-from-repetition-start note list (each lane's
// notes never overlap themselves, but different lanes' notes are
// otherwise unrelated); every lane repeats indefinitely over the same
// shared spanBeats, i.e. a lane's absolute note starts are
// n*spanBeats + note.startBeat for n = 0, 1, 2, ...
//
// wavFilePath/midiFilePath are derived from the chart's declared
// `name` field (chart directory + name + ".wav"/".mid"), not
// independently authored paths. The .mid file is optional - hasMidi is
// false (laneNotes/spanBeats left at their defaults) when it doesn't
// exist on disk, which is only a problem for a clip used in a [learn]
// section (rejected at load time); [break]/[reset]/[background] sections
// never touch MIDI data at all.
// How a [learn] clip's judging responds to a miss - see GameSession/
// SectionInstance for the actual state machine. Only meaningful for a
// clip used in a [learn] section; ignored entirely for break/reset/
// background usage.
enum class LearnMode
{
    // A streak of hitsRequired correct hits in a row locks the section
    // in permanently for the rest of its run - once reached, further
    // misses never un-reach it.
    Pass,
    // The same streak is reversible: starts already "passing," any miss
    // immediately drops it to "failing," and re-earning hitsRequired in
    // a row brings it back to "passing." Independent of the separate
    // 3-consecutive-miss clip-stop penalty, which applies the same way
    // in both modes.
    DontFail,
};

struct ChartClip
{
    // Stable identifier, also the file stem for wavFilePath/midiFilePath.
    // This is what section blocks reference via their `clip` field.
    std::wstring name;
    // Human-readable label only - never used for cross-referencing.
    std::wstring displayName;
    std::wstring wavFilePath;
    std::wstring midiFilePath;
    bool hasMidi = false;
    std::vector<LaneNote> laneNotes[kLaneCount];
    double spanBeats = 4.0;
    int hitsRequired = 16;
    // Both resolved at load time: the clip's own start_tolerance_ms/
    // release_tolerance_ms if it declared one, otherwise the song's
    // global default (ChartSong::startToleranceMs/releaseToleranceMs) -
    // downstream code never needs to know which one applied.
    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;
    double initVolume = 1.0; // volume while the player is still learning this clip
    double volume = 1.0;     // volume once it's locked in and looping automatically, or during break/background playback
    // Declared per-clip only (no song-level default, unlike the
    // tolerances above) - see LearnMode's own comment.
    LearnMode learnMode = LearnMode::Pass;
};

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
struct ChartSong
{
    std::wstring title;
    double bpm = 120.0;
    int beatsPerBar = 4;
    // Default press/release judging tolerance for any clip that doesn't
    // declare its own start_tolerance_ms/release_tolerance_ms override.
    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;
    std::vector<ChartClip> clips;
    std::vector<ChartSection> sections;
};

// Parses/validates a .chart text file into a ChartSong - the only way a
// chart ever becomes one; nothing else in this codebase constructs a
// ChartSong by hand.
class ChartFile
{
public:
    // Parses and validates a .chart text file. Returns false if the file
    // can't be opened or fails validation - outErrors then holds a
    // human-readable message for every problem found (unsupported fields,
    // wrong-typed or out-of-range values, a malformed time signature,
    // missing required fields, a referenced stem/MIDI file that doesn't
    // exist or can't be parsed, a duplicate clip name, a section
    // referencing an unknown clip, an unrecognized block header, or a
    // chart with no [learn]/[break]/[reset]/[background] blocks at all).
    // On success outErrors is empty and outSong is filled in.
    static bool Load(const std::wstring& chartFilePath, ChartSong& outSong, std::vector<std::wstring>& outErrors);
};
