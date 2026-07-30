#include "ChartMidi.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "MidiFile.h"

namespace
{

// Returns the lane index for a MIDI key number, or -1 if it isn't one of the tracked pitches.
int LaneForPitch(int keyNumber)
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (kLaneMidiPitches[lane] == keyNumber)
        {
            return lane;
        }
    }
    return -1;
}

// True once outData has at least one note on some lane.
bool HasAnyNotes(const MidiLaneData& data)
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (!data.lanes[lane].empty())
        {
            return true;
        }
    }
    return false;
}

} // namespace

// Loads midiFilePath and buckets its notes by lane, or fails with a description of the problem.
bool ChartMidi::LoadLaneNotes(const std::wstring& midiFilePath, MidiLaneData& outData, std::wstring& outError)
{
    // Read the raw bytes ourselves (wide-path-safe, same std::ifstream(wstring)
    // pattern AudioEngine.cpp already uses for WAV files) rather than handing
    // smf::MidiFile a filename directly - its filename overload takes a
    // narrow std::string, which isn't reliably Unicode-safe on this toolchain.
    std::ifstream file(midiFilePath.c_str(), std::ios::binary);
    if (!file)
    {
        outError = L"could not open file '" + midiFilePath + L"'";
        return false;
    }

    std::ostringstream bytes;
    bytes << file.rdbuf();
    std::istringstream input(bytes.str());

    smf::MidiFile midi;
    if (!midi.readSmf(input) || !midi.status())
    {
        outError = L"'" + midiFilePath + L"' isn't a readable Standard MIDI File";
        return false;
    }

    midi.absoluteTicks();
    midi.linkNotePairs();

    int ticksPerQuarterNote = midi.getTicksPerQuarterNote();
    if (ticksPerQuarterNote <= 0)
    {
        outError = L"'" + midiFilePath + L"' has an invalid or unsupported time division";
        return false;
    }

    int maxEndTick = 0;
    for (int track = 0; track < midi.getTrackCount(); ++track)
    {
        for (int i = 0; i < midi[track].getEventCount(); ++i)
        {
            smf::MidiEvent& event = midi[track][i];

            if (event.isEndOfTrack())
            {
                maxEndTick = std::max(maxEndTick, event.tick);
                continue;
            }

            if (!event.isNoteOn())
            {
                continue;
            }

            int lane = LaneForPitch(event.getKeyNumber());
            if (lane < 0)
            {
                continue;
            }

            if (!event.isLinked())
            {
                outError = L"'" + midiFilePath + L"' has a note-on (pitch " + std::to_wstring(event.getKeyNumber()) +
                            L") at tick " + std::to_wstring(event.tick) + L" with no matching note-off";
                return false;
            }

            if (event.getTickDuration() <= 0)
            {
                // A note-on immediately followed by its own note-off at the
                // same tick is valid MIDI but meaningless here - it could
                // never be pressed and released in time, so it's rejected
                // rather than silently producing an unplayable note.
                outError = L"'" + midiFilePath + L"' has a zero-length note (pitch " +
                            std::to_wstring(event.getKeyNumber()) + L") at tick " + std::to_wstring(event.tick);
                return false;
            }

            double startBeat = event.tick / static_cast<double>(ticksPerQuarterNote);
            double durationBeats = event.getTickDuration() / static_cast<double>(ticksPerQuarterNote);
            outData.lanes[lane].push_back({startBeat, durationBeats});

            maxEndTick = std::max(maxEndTick, event.tick + event.getTickDuration());
        }
    }

    if (!HasAnyNotes(outData))
    {
        outError = L"'" + midiFilePath + L"' contains no notes on any of the required lane pitches";
        return false;
    }

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        std::vector<LaneNote>& notes = outData.lanes[lane];
        std::sort(notes.begin(), notes.end(),
                  [](const LaneNote& a, const LaneNote& b) { return a.startBeat < b.startBeat; });

        // GameSession holds at most one press/hold per lane at a time, so
        // two notes on the same lane whose durations overlap are
        // ambiguous - which one is the player supposed to be holding? -
        // and would otherwise silently make the later note unplayable
        // instead of failing loudly at load time.
        for (size_t i = 0; i + 1 < notes.size(); ++i)
        {
            double thisEndBeat = notes[i].startBeat + notes[i].durationBeats;
            if (thisEndBeat > notes[i + 1].startBeat + 1e-6)
            {
                outError = L"'" + midiFilePath + L"' has overlapping notes on lane " + std::to_wstring(lane) +
                            L" (pitch " + std::to_wstring(kLaneMidiPitches[lane]) + L") around beat " +
                            std::to_wstring(notes[i + 1].startBeat);
                return false;
            }
        }
    }

    outData.totalBeats = maxEndTick / static_cast<double>(ticksPerQuarterNote);
    return true;
}
