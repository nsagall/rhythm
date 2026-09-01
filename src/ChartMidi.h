#pragma once

#include <string>
#include <vector>

#include "LaneConfig.h"  // c_LaneCount sizes MidiLaneData::lanes.

// One note extracted from a clip's MIDI file for a single lane: start and duration, both in beats.
// Ticks convert to beats via the file's division only; no tempo meta-events are read anywhere.
struct LaneNote
{
    double startBeat = 0.0;
    double durationBeats = 0.0;
};

// Every lane's notes (sorted by startBeat) plus the file's overall length in beats, which becomes
// the clip's spanBeats - so a repeating pattern loops on the author's bar boundary rather than
// shrinking to the last note's end.
struct MidiLaneData
{
    std::vector<LaneNote> lanes[c_LaneCount];
    double totalBeats = 0.0;
};

// Thin adapter over the vendored smf::MidiFile library: reads a Standard MIDI File and keeps only
// note-on/note-off pairs for the c_LaneMidiPitches pitches; every other pitch, channel, meta event,
// and track is ignored.
class ChartMidi
{
public:
    // Loads midiFilePath and buckets its notes by lane.
    //   midiFilePath - path to the .mid file.
    //   outData      - filled with the per-lane notes and total length.
    //   outError     - on failure, describes the problem (unreadable/malformed file, a note-on with
    //                  no matching note-off, or no notes on any tracked lane pitch).
    // Returns false on any of those failures.
    static bool LoadLaneNotes(const std::wstring& midiFilePath, MidiLaneData& outData, std::wstring& outError);
};
