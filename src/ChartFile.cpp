#include "ChartFile.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <sstream>

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

// Extracts the value of a "key:value" token, or "" if the token isn't for that key.
std::wstring ExtractTokenValue(const std::wstring& token, const std::wstring& key)
{
    std::wstring prefix = key + L":";
    if (token.rfind(prefix, 0) == 0)
    {
        return token.substr(prefix.size());
    }
    return L"";
}

// Builds the 0-indexed onset list for one bar of a named subdivision (whole/half/quarter/eighth/sixteenth).
std::vector<double> BuildSubdivisionOnsets(const std::wstring& subdivision, double barBeats)
{
    double step = 1.0;
    if (subdivision == L"whole")
    {
        step = 4.0;
    }
    else if (subdivision == L"half")
    {
        step = 2.0;
    }
    else if (subdivision == L"quarter")
    {
        step = 1.0;
    }
    else if (subdivision == L"eighth")
    {
        step = 0.5;
    }
    else if (subdivision == L"sixteenth")
    {
        step = 0.25;
    }

    std::vector<double> onsets;
    for (double beat = 0.0; beat < barBeats - 1e-9; beat += step)
    {
        onsets.push_back(beat);
    }
    return onsets;
}

// Parses a "pattern = ..." value into a 0-indexed onset list and the span (in beats) it repeats over.
// Three forms: a named subdivision ("quarter", optionally "every:N offset:K"),
// "bar beats:<1-indexed comma list>" (repeats every bar), or
// "explicit beats:<1-indexed comma list> span:<beats>" (repeats every span beats).
// Appends a descriptive entry to errors (prefixed with "Line N: context") for
// every problem found - an unrecognized pattern kind, an unsupported option,
// a non-numeric or out-of-range value - and returns false if the pattern
// couldn't be parsed into any usable onsets.
bool ParsePattern(const std::wstring& value, int beatsPerBar, std::vector<double>& outBeats, double& outSpanBeats,
                   const std::wstring& context, int lineNumber, std::vector<std::wstring>& errors)
{
    auto addError = [&](const std::wstring& message)
    {
        errors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L" " + message);
    };

    std::wstringstream ss(value);
    std::wstring firstToken;
    ss >> firstToken;

    if (firstToken == L"bar" || firstToken == L"explicit")
    {
        std::wstring beatsCsv;
        double span = beatsPerBar;
        bool ok = true;
        std::wstring token;
        while (ss >> token)
        {
            std::wstring beatsVal = ExtractTokenValue(token, L"beats");
            if (!beatsVal.empty())
            {
                beatsCsv = beatsVal;
                continue;
            }
            std::wstring spanVal = ExtractTokenValue(token, L"span");
            if (!spanVal.empty())
            {
                if (!TryParseStrictDouble(spanVal, span) || span <= 0.0)
                {
                    addError(L"pattern's span must be a positive number, got '" + spanVal + L"'");
                    ok = false;
                }
                continue;
            }
            addError(L"pattern has an unsupported option '" + token + L"'");
            ok = false;
        }

        if (beatsCsv.empty())
        {
            addError(L"pattern is missing its required 'beats:' list");
            return false;
        }

        std::wstringstream beatsStream(beatsCsv);
        std::wstring beatToken;
        std::vector<double> oneIndexed;
        while (std::getline(beatsStream, beatToken, L','))
        {
            std::wstring trimmed = Trim(beatToken);
            double beatValue;
            if (!TryParseStrictDouble(trimmed, beatValue))
            {
                addError(L"pattern has a non-numeric beat value '" + trimmed + L"'");
                ok = false;
                continue;
            }
            if (beatValue < 1.0)
            {
                addError(L"pattern beat values must be >= 1 (they're 1-indexed), got '" + trimmed + L"'");
                ok = false;
                continue;
            }
            oneIndexed.push_back(beatValue);
        }

        if (oneIndexed.empty())
        {
            return false;
        }

        outBeats.clear();
        for (double b : oneIndexed)
        {
            outBeats.push_back(b - 1.0); // authored as 1-indexed musical beats
        }
        outSpanBeats = span;
        return ok;
    }

    if (firstToken == L"whole" || firstToken == L"half" || firstToken == L"quarter" ||
        firstToken == L"eighth" || firstToken == L"sixteenth")
    {
        std::vector<double> baseOnsets = BuildSubdivisionOnsets(firstToken, beatsPerBar);

        int every = 1;
        int offset = 0;
        bool ok = true;
        std::wstring token;
        while (ss >> token)
        {
            std::wstring everyVal = ExtractTokenValue(token, L"every");
            if (!everyVal.empty())
            {
                if (!TryParseStrictInt(everyVal, every) || every <= 0)
                {
                    addError(L"pattern's 'every' must be a positive whole number, got '" + everyVal + L"'");
                    ok = false;
                    every = 1;
                }
                continue;
            }
            std::wstring offsetVal = ExtractTokenValue(token, L"offset");
            if (!offsetVal.empty())
            {
                if (!TryParseStrictInt(offsetVal, offset) || offset < 0)
                {
                    addError(L"pattern's 'offset' must be a non-negative whole number, got '" + offsetVal + L"'");
                    ok = false;
                    offset = 0;
                }
                continue;
            }
            addError(L"pattern has an unsupported option '" + token + L"'");
            ok = false;
        }

        outBeats.clear();
        for (int i = 0; i < static_cast<int>(baseOnsets.size()); ++i)
        {
            if (i >= offset && (i - offset) % every == 0)
            {
                outBeats.push_back(baseOnsets[i]);
            }
        }
        outSpanBeats = beatsPerBar;
        return ok;
    }

    addError(L"pattern '" + value +
             L"' isn't a recognized pattern kind (expected 'bar', 'explicit', 'whole', 'half', 'quarter', 'eighth', or 'sixteenth')");
    return false;
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
    std::wstring currentSection;
    ChartInstrument currentInstrument;
    bool haveInstrument = false;
    bool patternProvided = false;
    bool sawSongSection = false;
    int instrumentOrdinal = 0;

    auto instrumentContext = [&]()
    {
        std::wstring context = L"instrument #" + std::to_wstring(instrumentOrdinal + 1);
        if (!currentInstrument.name.empty())
        {
            context += L" (" + currentInstrument.name + L")";
        }
        return context;
    };

    auto flushInstrument = [&]()
    {
        if (!haveInstrument)
        {
            return;
        }
        std::wstring context = instrumentContext();
        ++instrumentOrdinal;

        if (currentInstrument.name.empty())
        {
            outErrors.push_back(context + L": missing required field 'name'");
        }
        if (currentInstrument.wavFilePath.empty())
        {
            outErrors.push_back(context + L": missing required field 'file'");
        }
        else
        {
            std::ifstream stemTest(currentInstrument.wavFilePath.c_str(), std::ios::binary);
            if (!stemTest)
            {
                outErrors.push_back(context + L": file '" + currentInstrument.wavFilePath + L"' does not exist");
            }
        }
        if (!patternProvided)
        {
            outErrors.push_back(context + L": missing required field 'pattern'");
        }

        song.instruments.push_back(currentInstrument);
        currentInstrument = ChartInstrument{};
        haveInstrument = false;
        patternProvided = false;
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
            flushInstrument();
            currentSection = section;
            if (currentSection == L"instrument")
            {
                haveInstrument = true;
            }
            else if (currentSection == L"song")
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

        if (currentSection == L"song")
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
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unsupported field '" + key + L"' in [song] section");
            }
        }
        else if (currentSection == L"instrument")
        {
            std::wstring context = instrumentContext();
            if (key == L"name")
            {
                if (value.empty())
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": name must not be empty");
                }
                else
                {
                    currentInstrument.name = value;
                }
            }
            else if (key == L"file")
            {
                if (value.empty())
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": file must not be empty");
                }
                else
                {
                    currentInstrument.wavFilePath = chartDir + value;
                }
            }
            else if (key == L"pattern")
            {
                std::vector<double> beats;
                double span = 0.0;
                bool ok = ParsePattern(value, song.beatsPerBar, beats, span, context, lineNumber, outErrors);
                patternProvided = true;
                if (ok)
                {
                    currentInstrument.patternBeats = std::move(beats);
                    std::sort(currentInstrument.patternBeats.begin(), currentInstrument.patternBeats.end());
                    currentInstrument.spanBeats = span;
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
                    currentInstrument.hitsRequired = hits;
                }
            }
            else if (key == L"tolerance_ms")
            {
                double tolerance;
                if (!TryParseStrictDouble(value, tolerance))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": tolerance_ms must be a number, got '" + value + L"'");
                }
                else if (tolerance <= 0.0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": tolerance_ms must be positive, got '" + value + L"'");
                }
                else
                {
                    currentInstrument.toleranceMs = tolerance;
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
                    currentInstrument.initVolume = volume;
                }
                else
                {
                    currentInstrument.volume = volume;
                }
            }
            else if (key == L"intro_bars" || key == L"outro_loops")
            {
                int count;
                if (!TryParseStrictInt(value, count))
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must be a whole number, got '" + value + L"'");
                }
                else if (count < 0)
                {
                    outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": " + context + L": " + key + L" must not be negative, got '" + value + L"'");
                }
                else if (key == L"intro_bars")
                {
                    currentInstrument.introBars = count;
                }
                else
                {
                    currentInstrument.outroLoops = count;
                }
            }
            else
            {
                outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": unsupported field '" + key + L"' in [instrument] section");
            }
        }
        else
        {
            outErrors.push_back(L"Line " + std::to_wstring(lineNumber) + L": field '" + key +
                                 L"' is not inside a recognized [song] or [instrument] section");
        }
    }

    flushInstrument();

    if (!sawSongSection)
    {
        outErrors.push_back(L"Chart is missing a required [song] section");
    }
    else if (song.bpm <= 0.0)
    {
        outErrors.push_back(L"[song] section is missing its required field 'bpm'");
    }
    if (song.instruments.empty())
    {
        outErrors.push_back(L"Chart must declare at least one [instrument] section");
    }

    if (!outErrors.empty())
    {
        return false;
    }

    outSong = std::move(song);
    return true;
}
