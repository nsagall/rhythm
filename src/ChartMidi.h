#pragma once

#include <string>
#include <vector>

#include "LaneConfig.h"

// One note extracted from a clip's MIDI file for a single lane:
// where it starts and how long it lasts, both in beats (quarter notes) -
// ticks convert to beats via the file's own division only; no tempo
// meta-events are read or applied anywhere, matching how quarter/eighth/
// sixteenth already mean beat-fractions elsewhere in this codebase.
struct LaneNote
{
    double startBeat = 0.0;
    double durationBeats = 0.0;
};

// Every lane's notes (independently sorted by startBeat) plus the file's
// overall length in beats - this becomes the clip's spanBeats, so a
// repeating pattern loops on the author's own bar boundary instead of
// shrinking to "last note's end" and swallowing trailing rest space.
struct MidiLaneData
{
    std::vector<LaneNote> lanes[c_LaneCount];
    double totalBeats = 0.0;
};

// Thin adapter over the vendored smf::MidiFile library (third_party/midifile):
// reads a Standard MIDI File and keeps only note-on/note-off pairs for the
// c_LaneCount pitches in c_LaneMidiPitches (LaneConfig.h) - every other pitch,
// channel, meta event, and track is ignored entirely once past the point of
// finding the overall track length.
class ChartMidi
{
public:
    // Loads midiFilePath and buckets its notes by lane. Returns false if the
    // file can't be read, isn't a well-formed Standard MIDI File, has a
    // note-on with no matching note-off, or has no notes on any tracked
    // lane pitch - outError then describes the problem, mirroring
    // ChartSong::Load's error-reporting style.
    static bool LoadLaneNotes(const std::wstring& midiFilePath, MidiLaneData& outData, std::wstring& outError);
};
