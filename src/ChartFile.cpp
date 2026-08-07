#include "ChartFile.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>

namespace
{

// Trims leading/trailing whitespace from a wide string.
std::wstring Trim(const std::wstring& s)
{
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos)
    {
        return L"";
    }
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Splits "key = value" into trimmed key/value strings. Returns false if there's no '='.
bool SplitKeyValue(const std::wstring& line, std::wstring& outKey, std::wstring& outValue)
{
    size_t eq = line.find(L'=');
    if (eq == std::wstring::npos)
    {
        return false;
    }
    outKey = Trim(line.substr(0, eq));
    outValue = Trim(line.substr(eq + 1));
    return true;
}

// If line is "[section]", extracts the section name and returns true.
bool ParseSectionHeader(const std::wstring& line, std::wstring& outSection)
{
    if (line.size() < 2 || line.front() != L'[' || line.back() != L']')
    {
        return false;
    }
    outSection = Trim(line.substr(1, line.size() - 2));
    return true;
}

// Returns the directory portion of a path (everything up to and including the last slash), or "" if none.
std::wstring GetDirectory(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L"";
    }
    return path.substr(0, slash + 1);
}

// Parses a whole number, rejecting anything that isn't purely digits (with
// an optional leading '-') - unlike std::stoi, which silently truncates
// "8.5" to 8 instead of rejecting it.
bool TryParseStrictInt(const std::wstring& text, int& outValue)
{
    std::wstring s = Trim(text);
    if (s.empty())
    {
        return false;
    }

    size_t digitsStart = (s[0] == L'-') ? 1 : 0;
    if (digitsStart >= s.size())
    {
        return false;
    }
    for (size_t i = digitsStart; i < s.size(); ++i)
    {
        if (!std::iswdigit(s[i]))
        {
            return false;
        }
    }

    try
    {
        size_t consumed = 0;
        long value = std::stol(s, &consumed);
        if (consumed != s.size())
        {
            return false;
        }
        outValue = static_cast<int>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Parses a floating-point number, rejecting anything with trailing garbage,
// stray characters (so "inf"/"nan"/hex-float forms can't sneak through),
// or a non-finite result.
bool TryParseStrictDouble(const std::wstring& text, double& outValue)
{
    std::wstring s = Trim(text);
    if (s.empty())
    {
        return false;
    }

    for (wchar_t c : s)
    {
        bool isAllowed = std::iswdigit(c) || c == L'.' || c == L'-' || c == L'+' || c == L'e' || c == L'E';
        if (!isAllowed)
        {
            return false;
        }
    }

    try
    {
        size_t consumed = 0;
        double value = std::stod(s, &consumed);
        if (consumed != s.size() || !std::isfinite(value))
        {
            return false;
        }
        outValue = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Parses "N/D" into beatsPerBar (N), accepting only a musically sensible
// time signature: a single '/', a positive whole-number numerator, and a
// denominator that's an actual note value (1, 2, 4, 8, 16, or 32).
bool ParseTimeSignature(const std::wstring& value, int& outBeatsPerBar)
{
    size_t slash = value.find(L'/');
    if (slash == std::wstring::npos || value.find(L'/', slash + 1) != std::wstring::npos)
    {
        return false;
    }

    int numerator;
    int denominator;
    if (!TryParseStrictInt(Trim(value.substr(0, slash)), numerator) ||
        !TryParseStrictInt(Trim(value.substr(slash + 1)), denominator))
    {
        return false;
    }
    if (numerator < 1 || numerator > 32)
    {
        return false;
    }

    static const int kValidDenominators[] = {1, 2, 4, 8, 16, 32};
    bool denominatorOk = std::find(std::begin(kValidDenominators), std::end(kValidDenominators), denominator) !=
                          std::end(kValidDenominators);
    if (!denominatorOk)
    {
        return false;
    }

    outBeatsPerBar = numerator;
    return true;
}

// Rounds totalBeats up to the next whole multiple of beatsPerBar (at least
// one full bar), so a repeating MIDI pattern always tiles on a bar
// boundary. Without this, a DAW export that trims its MIDI region to end
// right after the last note (rather than at the loop's actual bar
// boundary) would tile at that arbitrary length and silently drift out of
// phase with the audio after the first loop - not a crash, just wrong,
// which is exactly the kind of case that needs handling automatically
// rather than left for the author to notice by ear.
double AlignToBarBoundary(double totalBeats, int beatsPerBar)
{
    double bars = totalBeats / static_cast<double>(beatsPerBar);
    double alignedBars = std::ceil(bars - 1e-6);
    if (alignedBars < 1.0)
    {
        alignedBars = 1.0;
    }
    return alignedBars * beatsPerBar;
}

} // namespace

// Parses and validates a .chart text file. Returns false if the file can't be opened or fails validation.
bool ChartFile::Load(const std::wstring& chartFilePath, ChartSong& outSong, std::vector<std::wstring>& outErrors)
{
    outErrors.clear();

    std::wifstream file(chartFilePath.c_str());
    if (!file)
    {
        outErrors.push_back(L"Could not open file: " + chartFilePath);
        return false;
    }

    std::wstring chartDir = GetDirectory(chartFilePath);

    ChartSong song;
    std::wstring currentBlockKind;

    ChartClip currentClip;
    bool haveClip = false;
    bool startToleranceProvided = false;
    bool releaseToleranceProvided = false;
    int clipOrdinal = 0;

    ChartSection currentSectionData;
    bool haveSection = false;
    bool clipNameProvided = false;
    std::wstring currentClipNameRaw;
    int sectionOrdinal = 0;

    bool sawSongSection = false;

    auto clipContext = [&]()
    {
        std::wstring context = L"clip #" + std::to_wstring(clipOrdinal + 1);
        if (!currentClip.name.empty())
        {
            context += L" (" + currentClip.name + L")";
        }
        return context;
    };

    auto sectionContext = [&]()
    {
        std::wstring context = L"[" + currentBlockKind + L"] #" + std::to_wstring(sectionOrdinal + 1);
        if (clipNameProvided && !currentClipNameRaw.empty())
        {
            context += L" (" + currentClipNameRaw + L")";
        }
        return context;
    };

    auto flushClip = [&]()
    {
        if (!haveClip)
        {
            return;
        }
        std::wstring context = clipContext();
        ++clipOrdinal;

        if (currentClip.displayName.empty())
        {
            outErrors.push_back(context + L": missing required field 'display_name'");
        }

        if (currentClip.name.empty())
        {
            // wavFilePath/midiFilePath are both derived from the single
            // `name` field together, so an empty name means it was never
            // given at all - nothing else to check.
            outErrors.push_back(context + L": missing required field 'name'");
        }
        else
        {
            for (const ChartClip& existing : song.clips)
            {
                if (existing.name == currentClip.name)
                {
                    outErrors.push_back(context + L": a clip named '" + currentClip.name + L"' was already declared");
                    break;
                }
            }

            std::ifstream stemTest(currentClip.wavFilePath.c_str(), std::ios::binary);
            if (!stemTest)
            {
                outErrors.push_back(context + L": file '" + currentClip.wavFilePath + L"' does not exist");
            }

            std::ifstream midiTest(currentClip.midiFilePath.c_str(), std::ios::binary);
            if (!midiTest)
            {
                // No .mid file next to the .wav - fine, this clip just
                // can't be used in a `learn` section (checked in
                // flushSection once its sections are known).
                currentClip.hasMidi = false;
            }
            else
            {
                MidiLaneData midiData;
                std::wstring midiError;
                if (!ChartMidi::LoadLaneNotes(currentClip.midiFilePath, midiData, midiError))
                {
                    outErrors.push_back(context + L": " + midiError);
                }
                else
                {
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        currentClip.laneNotes[lane] = std::move(midiData.lanes[lane]);
                    }
                    currentClip.spanBeats = AlignToBarBoundary(midiData.totalBeats, song.beatsPerBar);
                    currentClip.hasMidi = true;
                }
            }
        }

        if (!startToleranceProvided)
        {
            currentClip.startToleranceMs = song.startToleranceMs;
        }
        if (!releaseToleranceProvided)
        {
            currentClip.releaseToleranceMs = song.releaseToleranceMs;
        }

        song.clips.push_back(currentClip);
        currentClip = ChartClip{};
        haveClip = false;
        startToleranceProvided = false;
        releaseToleranceProvided = false;
    };

    auto flushSection = [&]()
    {
        if (!haveSection)
        {
            return;
        }
        std::wstring context = sectionContext();
        ++sectionOrdinal;

        bool clipGiven = clipNameProvided && !currentClipNameRaw.empty();

        switch (currentSectionData.kind)
        {
            case SectionKind::Learn:
                if (!clipGiven)
                {
                    outErrors.push_back(context + L": [learn] requires a non-empty 'clip'");
                }
                break;
            case SectionKind::Break:
                if (!clipGiven)
                {
                    outErrors.push_back(context + L": [break] requires a non-empty 'clip'");
                }
                break;
            case SectionKind::Reset:
                if (clipGiven)
                {
                    outErrors.push_back(context + L": [reset] must not specify a non-empty 'clip' (use [break] instead)");
                }
                break;
            case SectionKind::Background:
                if (!clipGiven)
                {
                    outErrors.push_back(context + L": [background] requires a non-empty 'clip'");
                }
                break;
        }

        if (clipGiven)
        {
            int foundIndex = -1;
            for (size_t i = 0; i < song.clips.size(); ++i)
            {
                if (song.clips[i].name == currentClipNameRaw)
                {
                    foundIndex = static_cast<int>(i);
                    break;
                }
            }
            if (foundIndex < 0)
            {
                outErrors.push_back(context + L": clip '" + currentClipNameRaw + L"' does not match any declared [clip]");
            }
            else
            {
                currentSectionData.clipIndex = foundIndex;
                if (currentSectionData.kind == SectionKind::Learn && !song.clips[foundIndex].hasMidi)
                {
                    outErrors.push_back(context + L": clip '" + currentClipNameRaw +
                                         L"' has no .mid file and can't be used in a [learn] section");
                }
            }
        }
        else
        {
            currentSectionData.clipIndex = -1;
        }

        song.sections.push_back(currentSectionData);
        currentSectionData = ChartSection{};
        haveSection = false;
        clipNameProvided = false;
        currentClipNameRaw.clear();
    };

    auto flushCurrentBlock = [&]()
    {
        flushClip();
        flushSection();
    };

    std::wstring line;
    int lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line.front() == L';' || line.front() == L'#')
        {
            continue;
        }

        std::wstring section;
        if (ParseSectionHeader(line, section))
        {
            flushCurrentBlock();
            currentBlockKind = section;
            if (currentBlockKind == L"clip")
            {
                haveClip = true;
            }
            else if (currentBlockKind == L"learn")
            {
                haveSection = true;
                currentSectionData.kind = SectionKind::Learn;
            }
            else if (currentBlockKind == L"break")
            {
                haveSection = true;
                currentSectionData.kind = SectionKind::Break;
            }
            else if (currentBlockKind == L"reset")
            {
                haveSection = true;
                currentSectionData.kind = SectionKind::Reset;
            }
            else if (currentBlockKind == L"background")
            {
                haveSection = true;
                currentSectionData.kind = SectionKind::Background;
            }
            else if (currentBlockKind == L"song")
            {
                sawSongSection = true;
            }
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unknown section '[" + section + L"]'");
            }
            continue;
        }

        std::wstring key;
        std::wstring value;
        if (!SplitKeyValue(line, key, value))
        {
            outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": couldn't parse line (expected 'key = value')");
            continue;
        }

        if (currentBlockKind == L"song")
        {
            if (key == L"title")
            {
                song.title = value;
            }
            else if (key == L"bpm")
            {
                double bpm;
                if (!TryParseStrictDouble(value, bpm))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": bpm must be a number, got '" + value + L"'");
                }
                else if (bpm <= 0.0 || bpm > 999.0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": bpm must be between 1 and 999, got '" + value + L"'");
                }
                else
                {
                    song.bpm = bpm;
                }
            }
            else if (key == L"time_signature")
            {
                int beatsPerBar;
                if (!ParseTimeSignature(value, beatsPerBar))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": time_signature '" + value +
                                         L"' is not a valid time signature (expected e.g. '4/4')");
                }
                else
                {
                    song.beatsPerBar = beatsPerBar;
                }
            }
            else if (key == L"start_tolerance_ms" || key == L"release_tolerance_ms")
            {
                double tolerance;
                if (!TryParseStrictDouble(value, tolerance))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + key + L" must be a number, got '" + value + L"'");
                }
                else if (tolerance <= 0.0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + key + L" must be positive, got '" + value + L"'");
                }
                else if (key == L"start_tolerance_ms")
                {
                    song.startToleranceMs = tolerance;
                }
                else
                {
                    song.releaseToleranceMs = tolerance;
                }
            }
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unsupported field '" + key + L"' in [song] section");
            }
        }
        else if (currentBlockKind == L"clip")
        {
            std::wstring context = clipContext();
            if (key == L"display_name")
            {
                if (value.empty())
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": display_name must not be empty");
                }
                else
                {
                    currentClip.displayName = value;
                }
            }
            else if (key == L"name")
            {
                if (value.empty())
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": name must not be empty");
                }
                else
                {
                    currentClip.name = value;
                    currentClip.wavFilePath = chartDir + value + L".wav";
                    currentClip.midiFilePath = chartDir + value + L".mid";
                }
            }
            else if (key == L"hits_required")
            {
                int hits;
                if (!TryParseStrictInt(value, hits))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": hits_required must be a whole number, got '" + value + L"'");
                }
                else if (hits <= 0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": hits_required must be positive, got '" + value + L"'");
                }
                else
                {
                    currentClip.hitsRequired = hits;
                }
            }
            else if (key == L"start_tolerance_ms" || key == L"release_tolerance_ms")
            {
                double tolerance;
                if (!TryParseStrictDouble(value, tolerance))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must be a number, got '" + value + L"'");
                }
                else if (tolerance <= 0.0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must be positive, got '" + value + L"'");
                }
                else if (key == L"start_tolerance_ms")
                {
                    currentClip.startToleranceMs = tolerance;
                    startToleranceProvided = true;
                }
                else
                {
                    currentClip.releaseToleranceMs = tolerance;
                    releaseToleranceProvided = true;
                }
            }
            else if (key == L"init_volume" || key == L"volume")
            {
                double volume;
                if (!TryParseStrictDouble(value, volume))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must be a number, got '" + value + L"'");
                }
                else if (volume < 0.0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must not be negative, got '" + value + L"'");
                }
                else if (key == L"init_volume")
                {
                    currentClip.initVolume = volume;
                }
                else
                {
                    currentClip.volume = volume;
                }
            }
            else if (key == L"learn_mode")
            {
                if (value == L"pass")
                {
                    currentClip.learnMode = LearnMode::Pass;
                }
                else if (value == L"dontfail")
                {
                    currentClip.learnMode = LearnMode::DontFail;
                }
                else
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": learn_mode must be 'pass' or 'dontfail', got '" + value + L"'");
                }
            }
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unsupported field '" + key + L"' in [clip] section");
            }
        }
        else if (currentBlockKind == L"learn" || currentBlockKind == L"break" || currentBlockKind == L"reset" ||
                 currentBlockKind == L"background")
        {
            std::wstring context = sectionContext();
            if (key == L"clip")
            {
                currentClipNameRaw = value;
                clipNameProvided = true;
            }
            else if (key == L"loop_count")
            {
                int count;
                if (!TryParseStrictInt(value, count))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": loop_count must be a whole number, got '" + value + L"'");
                }
                else if (count < 1)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": loop_count must be at least 1, got '" + value + L"'");
                }
                else
                {
                    currentSectionData.loopCount = count;
                }
            }
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unsupported field '" + key + L"' in [" + currentBlockKind + L"] section");
            }
        }
        else
        {
            outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": field '" + key +
                                 L"' is not inside a recognized [song], [clip], [learn], [break], [reset], or [background] section");
        }
    }

    flushCurrentBlock();

    if (!sawSongSection)
    {
        outErrors.push_back(L"Chart is missing a required [song] section");
    }
    else if (song.bpm <= 0.0)
    {
        outErrors.push_back(L"[song] section is missing its required field 'bpm'");
    }
    if (song.sections.empty())
    {
        outErrors.push_back(L"Chart must declare at least one [learn], [break], [reset], or [background] block");
    }

    if (!outErrors.empty())
    {
        return false;
    }

    outSong = std::move(song);
    return true;
}

