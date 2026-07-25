#pragma once

#include <string>
#include <vector>

// One instrument layer in a chart: its stem file, its rhythmic pattern, and
// the thresholds needed to lock it in. patternBeats holds 0-indexed onset
// positions within one repetition of length spanBeats (e.g. spanBeats=4 for
// a single 4/4 bar); the pattern repeats indefinitely as
// n*spanBeats + patternBeats[i] for n = 0, 1, 2, ...
struct ChartInstrument
{
    std::wstring name;
    std::wstring wavFilePath;
    std::vector<double> patternBeats;
    double spanBeats = 4.0;
    int hitsRequired = 16;
    double toleranceMs = 120.0;
    double initVolume = 1.0; // volume while the player is still learning this instrument
    double volume = 1.0;     // volume once it's locked in and looping automatically
    int introBars = 0;       // if > 0, this many bars play automatically (no tap needed) before dots/judging start
    int outroLoops = 0;      // if > 0, this many extra full loops play after locking in before the next instrument joins
};

// A full song: tempo/time signature plus an ordered list of instruments,
// introduced one at a time in the order they appear in the file.
struct ChartSong
{
    std::wstring title;
    double bpm = 120.0;
    int beatsPerBar = 4;
    std::vector<ChartInstrument> instruments;
};

class ChartFile
{
public:
    // Parses a .chart text file. Returns false if the file can't be read
    // or doesn't contain at least one valid instrument.
    static bool Load(const std::wstring& chartFilePath, ChartSong& outSong);
};
