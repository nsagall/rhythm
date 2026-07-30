#pragma once

// Single source of truth for how many lanes this build supports, so every
// other kLaneCount-shaped thing (the MIDI pitch table, per-lane judging
// arrays, rendering columns, keyboard bindings) is sized off this one
// constant instead of scattered hardcoded 4s - extending to more lanes
// later means only touching this file plus MainWindow's key table.
constexpr int kLaneCount = 4;

// The exact MIDI note numbers read out of an instrument's midi_file, left
// to right, lane 0..3 - all other note numbers in the file are ignored.
// This is a single global constant, not per-chart/per-instrument
// configurable. C4/D4/E4/F4 - but note Ableton Live (the DAW used to
// author this project's charts) numbers octaves one lower than the
// "middle C = C4" convention most other software/MIDI docs use, so
// Ableton's own "C4" is MIDI 72, not 60. These values are chosen to match
// what Ableton's piano roll displays as C4/D4/E4/F4, since that's the
// convention chart authors actually see.
constexpr int kLaneMidiPitches[kLaneCount] = { 72, 74, 76, 77 };

// How many beats ahead of the judge line a note spawns at the top edge of
// the play area - i.e. how long, in beats, a note is visible scrolling
// down before it must be hit. Shared between NoteLane (which uses it to
// place notes on screen) and GameSession (which uses it to guarantee a
// locked-in learn section always leaves at least this much real time
// before the next section, so the next section's notes get their full
// on-screen travel time instead of popping in late/partway down the lane).
constexpr double kNoteFallBeats = 3.0;
