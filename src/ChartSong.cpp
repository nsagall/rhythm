#include "ChartSong.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>

#include "ChartTextUtil.h"

namespace
{

using ChartTextUtil::GetDirectory;
using ChartTextUtil::Trim;

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

// Parses a strict whole number: only digits with an optional leading '-'. Unlike std::stoi, rejects
// fractional input rather than truncating it. Returns false on any invalid input.
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

// Parses a strict floating-point number, rejecting trailing garbage, stray characters (so
// "inf"/"nan"/hex-float forms can't sneak through), or a non-finite result. Returns false on any
// invalid input.
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

// Parses "N/D" into outBeatsPerBar (N) and outDenominator (D). Accepts only a single '/', a
// positive numerator, and a denominator that's a real note value (1, 2, 4, 8, 16, or 32). Returns
// false otherwise.
bool ParseTimeSignature(const std::wstring& value, int& outBeatsPerBar, int& outDenominator)
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

    static const int c_ValidDenominators[] = {1, 2, 4, 8, 16, 32};
    bool denominatorOk = std::find(std::begin(c_ValidDenominators), std::end(c_ValidDenominators), denominator) !=
                          std::end(c_ValidDenominators);
    if (!denominatorOk)
    {
        return false;
    }

    outBeatsPerBar = numerator;
    outDenominator = denominator;
    return true;
}

// Rounds totalBeats up to the next whole multiple of beatsPerBar (at least one full bar), so a
// repeating MIDI pattern always tiles on a bar boundary. A DAW export trimmed to end right after
// the last note would otherwise tile at that length and drift out of phase with the audio.
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

bool ChartSong::Load(const std::wstring& chartFilePath, std::vector<std::wstring>& outErrors)
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
        if (!currentClip.m_name.empty())
        {
            context += L" (" + currentClip.m_name + L")";
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

        if (currentClip.m_displayName.empty())
        {
            outErrors.push_back(context + L": missing required field 'display_name'");
        }

        if (currentClip.m_name.empty())
        {
            // wav/midi paths are both derived from `name`, so an empty name means it was never given.
            outErrors.push_back(context + L": missing required field 'name'");
        }
        else
        {
            for (const ChartClip& existing : song.m_clips)
            {
                if (existing.m_name == currentClip.m_name)
                {
                    outErrors.push_back(context + L": a clip named '" + currentClip.m_name + L"' was already declared");
                    break;
                }
            }

            std::ifstream stemTest(currentClip.m_wavFilePath.c_str(), std::ios::binary);
            if (!stemTest)
            {
                outErrors.push_back(context + L": file '" + currentClip.m_wavFilePath + L"' does not exist");
            }

            std::ifstream midiTest(currentClip.m_midiFilePath.c_str(), std::ios::binary);
            if (!midiTest)
            {
                // No .mid file - fine, but this clip can't be used in a [learn] section
                // (checked in flushSection).
                currentClip.m_hasMidi = false;
            }
            else
            {
                MidiLaneData midiData;
                std::wstring midiError;
                if (!ChartMidi::LoadLaneNotes(currentClip.m_midiFilePath, midiData, midiError))
                {
                    outErrors.push_back(context + L": " + midiError);
                }
                else
                {
                    for (int lane = 0; lane < c_LaneCount; ++lane)
                    {
                        currentClip.m_laneNotes[lane] = std::move(midiData.lanes[lane]);
                    }
                    currentClip.m_spanBeats = AlignToBarBoundary(midiData.totalBeats, song.m_beatsPerBar);
                    currentClip.m_hasMidi = true;
                }
            }
        }

        if (!startToleranceProvided)
        {
            currentClip.m_startToleranceMs = song.m_startToleranceMs;
        }
        if (!releaseToleranceProvided)
        {
            currentClip.m_releaseToleranceMs = song.m_releaseToleranceMs;
        }

        song.m_clips.push_back(currentClip);
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
            for (size_t i = 0; i < song.m_clips.size(); ++i)
            {
                if (song.m_clips[i].m_name == currentClipNameRaw)
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
                if (currentSectionData.kind == SectionKind::Learn && !song.m_clips[foundIndex].m_hasMidi)
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

        song.m_sections.push_back(currentSectionData);
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
                song.m_title = value;
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
                    song.m_bpm = bpm;
                }
            }
            else if (key == L"time_signature")
            {
                int beatsPerBar;
                int denominator;
                if (!ParseTimeSignature(value, beatsPerBar, denominator))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": time_signature '" + value +
                                         L"' is not a valid time signature (expected e.g. '4/4')");
                }
                else
                {
                    song.m_beatsPerBar = beatsPerBar;
                    song.m_timeSignatureDenominator = denominator;
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
                    song.m_startToleranceMs = tolerance;
                }
                else
                {
                    song.m_releaseToleranceMs = tolerance;
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
                    currentClip.m_displayName = value;
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
                    currentClip.m_name = value;
                    currentClip.m_wavFilePath = chartDir + value + L".wav";
                    currentClip.m_midiFilePath = chartDir + value + L".mid";
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
                    currentClip.m_hitsRequired = hits;
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
                    currentClip.m_startToleranceMs = tolerance;
                    startToleranceProvided = true;
                }
                else
                {
                    currentClip.m_releaseToleranceMs = tolerance;
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
                    currentClip.m_initVolume = volume;
                }
                else
                {
                    currentClip.m_volume = volume;
                }
            }
            else if (key == L"learn_mode")
            {
                if (value == L"pass")
                {
                    currentClip.m_learnMode = LearnMode::Pass;
                }
                else if (value == L"dontfail")
                {
                    currentClip.m_learnMode = LearnMode::DontFail;
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
    else if (song.m_bpm <= 0.0)
    {
        outErrors.push_back(L"[song] section is missing its required field 'bpm'");
    }
    if (song.m_sections.empty())
    {
        outErrors.push_back(L"Chart must declare at least one [learn], [break], [reset], or [background] block");
    }

    if (!outErrors.empty())
    {
        return false;
    }

    *this = std::move(song);
    return true;
}
