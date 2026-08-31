#pragma once

// Single source of truth for how many lanes this build supports; every other c_LaneCount-shaped
// thing sizes off this. Extending to more lanes means touching only this file and MainWindow's key
// table.
constexpr int c_LaneCount = 4;

// The exact MIDI note numbers read out of a clip's midi_file, left to right, lane 0..3; all other
// notes are ignored. C4/D4/E4/F4 as Ableton Live's piano roll displays them - Ableton numbers
// octaves one lower than the "middle C = C4" convention, so its "C4" is MIDI 72, not 60.
constexpr int c_LaneMidiPitches[c_LaneCount] = { 72, 74, 76, 77 };

// How many beats a note is visible scrolling down before it must be hit. Shared between NoteLane
// (to place notes on screen) and GameSession (to guarantee a locked-in learn section leaves at
// least this much real time before the next section, so its notes get their full travel time).
constexpr double c_NoteFallBeats = 4.0;
