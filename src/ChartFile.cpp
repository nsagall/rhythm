#include "ChartFile.h"

#include <algorithm>
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

// Splits a comma-separated list of doubles, e.g. "1,1.5,2".
std::vector<double> SplitDoubleList(const std::wstring& csv)
{
    std::vector<double> values;
    std::wstringstream ss(csv);
    std::wstring token;
    while (std::getline(ss, token, L','))
    {
        values.push_back(std::stod(Trim(token)));
    }
    return values;
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
bool ParsePattern(const std::wstring& value, int beatsPerBar, std::vector<double>& outBeats, double& outSpanBeats)
{
    std::wstringstream ss(value);
    std::wstring firstToken;
    ss >> firstToken;

    if (firstToken == L"bar" || firstToken == L"explicit")
    {
        std::wstring beatsCsv;
        double span = beatsPerBar;
        std::wstring token;
        while (ss >> token)
        {
            std::wstring beatsVal = ExtractTokenValue(token, L"beats");
            if (!beatsVal.empty())
            {
                beatsCsv = beatsVal;
            }
            std::wstring spanVal = ExtractTokenValue(token, L"span");
            if (!spanVal.empty())
            {
                span = std::stod(spanVal);
            }
        }
        if (beatsCsv.empty())
        {
            return false;
        }

        std::vector<double> oneIndexed = SplitDoubleList(beatsCsv);
        outBeats.clear();
        for (double b : oneIndexed)
        {
            outBeats.push_back(b - 1.0); // authored as 1-indexed musical beats
        }
        outSpanBeats = span;
        return true;
    }

    if (firstToken == L"whole" || firstToken == L"half" || firstToken == L"quarter" ||
        firstToken == L"eighth" || firstToken == L"sixteenth")
    {
        std::vector<double> baseOnsets = BuildSubdivisionOnsets(firstToken, beatsPerBar);

        int every = 1;
        int offset = 0;
        std::wstring token;
        while (ss >> token)
        {
            std::wstring everyVal = ExtractTokenValue(token, L"every");
            if (!everyVal.empty())
            {
                every = std::stoi(everyVal);
            }
            std::wstring offsetVal = ExtractTokenValue(token, L"offset");
            if (!offsetVal.empty())
            {
                offset = std::stoi(offsetVal);
            }
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
        return true;
    }

    return false;
}

} // namespace

// Parses a .chart text file. Returns false if the file can't be read or doesn't contain at least one valid instrument.
bool ChartFile::Load(const std::wstring& chartFilePath, ChartSong& outSong)
{
    std::wifstream file(chartFilePath.c_str());
    if (!file)
    {
        return false;
    }

    std::wstring chartDir = GetDirectory(chartFilePath);

    ChartSong song;
    std::wstring currentSection;
    ChartInstrument currentInstrument;
    bool haveInstrument = false;

    auto flushInstrument = [&]()
    {
        if (haveInstrument)
        {
            song.instruments.push_back(currentInstrument);
            currentInstrument = ChartInstrument{};
            haveInstrument = false;
        }
    };

    std::wstring line;
    while (std::getline(file, line))
    {
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
            continue;
        }

        std::wstring key;
        std::wstring value;
        if (!SplitKeyValue(line, key, value))
        {
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
                song.bpm = std::stod(value);
            }
            else if (key == L"time_signature")
            {
                size_t slash = value.find(L'/');
                std::wstring numerator = slash == std::wstring::npos ? value : value.substr(0, slash);
                song.beatsPerBar = std::stoi(numerator);
            }
        }
        else if (currentSection == L"instrument")
        {
            if (key == L"name")
            {
                currentInstrument.name = value;
            }
            else if (key == L"file")
            {
                currentInstrument.wavFilePath = chartDir + value;
            }
            else if (key == L"pattern")
            {
                ParsePattern(value, song.beatsPerBar, currentInstrument.patternBeats, currentInstrument.spanBeats);
                std::sort(currentInstrument.patternBeats.begin(), currentInstrument.patternBeats.end());
            }
            else if (key == L"hits_required")
            {
                currentInstrument.hitsRequired = std::stoi(value);
            }
            else if (key == L"tolerance_ms")
            {
                currentInstrument.toleranceMs = std::stod(value);
            }
        }
    }

    flushInstrument();

    if (song.instruments.empty() || song.bpm <= 0.0)
    {
        return false;
    }

    outSong = std::move(song);
    return true;
}
