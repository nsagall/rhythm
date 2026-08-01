#include "EditorChartIO.h"

#include <windows.h>

#include <cwchar>
#include <fstream>

namespace
{

// Mirrors ChartFile.cpp's own Trim/GetDirectory helpers (those are
// file-local to ChartFile.cpp, not exported) so this file's raw-text
// time-signature-denominator scan and path derivation stay byte-for-byte
// consistent with how the real parser behaves.
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

std::wstring GetDirectory(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L"";
    }
    return path.substr(0, slash + 1);
}

// Formats a double as plain fixed-point text with trailing zeros (and a
// bare trailing '.') trimmed off, e.g. 120.0 -> "120", 87.5 -> "87.5" -
// deliberately never scientific notation, purely for readability of the
// saved file (ChartFile::Load's TryParseStrictDouble would accept either
// form).
std::wstring FormatDouble(double value)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.6f", value);
    std::wstring s(buf);
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos)
    {
        size_t lastNonZero = s.find_last_not_of(L'0');
        if (lastNonZero == dot)
        {
            --lastNonZero;
        }
        s.erase(lastNonZero + 1);
    }
    return s;
}

// Best-effort, independent single-pass scan for a `time_signature = N/D`
// line's denominator - ChartFile::Load validates but discards it, so
// there's no other way to recover it for a faithful reload. Returns 4
// (the common default) if the file can't be opened or no such line parses.
int ReadRawTimeSignatureDenominator(const std::wstring& chartFilePath)
{
    std::wifstream file(chartFilePath.c_str());
    if (!file)
    {
        return 4;
    }

    std::wstring line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
        {
            continue;
        }
        if (Trim(line.substr(0, eq)) != L"time_signature")
        {
            continue;
        }
        std::wstring value = Trim(line.substr(eq + 1));
        size_t slash = value.find(L'/');
        if (slash == std::wstring::npos)
        {
            continue;
        }
        try
        {
            return std::stoi(Trim(value.substr(slash + 1)));
        }
        catch (...)
        {
            continue;
        }
    }
    return 4;
}

const wchar_t* SectionKindHeader(SectionKind kind)
{
    switch (kind)
    {
        case SectionKind::Learn:
            return L"learn";
        case SectionKind::Break:
            return L"break";
        case SectionKind::Reset:
            return L"reset";
        case SectionKind::Background:
            return L"background";
    }
    return L"learn";
}

} // namespace

namespace EditorChartIO
{

bool LoadIntoDocument(const std::wstring& chartFilePath, EditorDocument& outDoc, std::vector<std::wstring>& outErrors)
{
    ChartSong song;
    if (!ChartFile::Load(chartFilePath, song, outErrors))
    {
        return false;
    }

    EditorDocument doc;
    doc.chartFilePath = chartFilePath;
    doc.folderPath = GetDirectory(chartFilePath);
    doc.title = song.title;
    doc.bpm = song.bpm;
    doc.beatsPerBar = song.beatsPerBar;
    doc.timeSignatureDenominator = ReadRawTimeSignatureDenominator(chartFilePath);
    doc.startToleranceMs = song.startToleranceMs;
    doc.releaseToleranceMs = song.releaseToleranceMs;

    doc.clips.reserve(song.clips.size());
    for (const ChartClip& clip : song.clips)
    {
        EditorClip editorClip;
        editorClip.id = doc.nextClipId++;
        editorClip.name = clip.name;
        editorClip.displayName = clip.displayName;
        // ChartFile::Load already guarantees the .wav exists (it's a hard
        // load error otherwise), so a clip that made it into `song.clips`
        // always has one.
        editorClip.hasWav = true;
        editorClip.hasMidi = clip.hasMidi;
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            editorClip.laneNotes[lane] = clip.laneNotes[lane];
        }
        editorClip.spanBeats = clip.spanBeats;
        editorClip.hitsRequired = clip.hitsRequired;
        // "Equals the song default" heuristic: ChartFile::Load assigns a
        // non-overriding clip's tolerance from song.startToleranceMs
        // verbatim, so an unset override is bit-identical to the song
        // value here; an explicit override that happens to match the
        // default numerically will display as inherited, a documented,
        // functionally-harmless limitation (the resolved value is the same
        // either way).
        editorClip.startToleranceMs.isOverridden = (clip.startToleranceMs != song.startToleranceMs);
        editorClip.startToleranceMs.value = clip.startToleranceMs;
        editorClip.releaseToleranceMs.isOverridden = (clip.releaseToleranceMs != song.releaseToleranceMs);
        editorClip.releaseToleranceMs.value = clip.releaseToleranceMs;
        editorClip.initVolume = clip.initVolume;
        editorClip.volume = clip.volume;
        doc.clips.push_back(std::move(editorClip));
    }

    doc.sections.reserve(song.sections.size());
    for (const ChartSection& section : song.sections)
    {
        EditorSection editorSection;
        editorSection.id = doc.nextSectionId++;
        editorSection.kind = section.kind;
        editorSection.clipId = (section.clipIndex >= 0 && static_cast<size_t>(section.clipIndex) < doc.clips.size())
                                    ? doc.clips[static_cast<size_t>(section.clipIndex)].id
                                    : -1;
        editorSection.loopCount = section.loopCount;
        doc.sections.push_back(editorSection);
    }

    doc.dirty = false;
    doc.docVersion = 0;
    outDoc = std::move(doc);
    return true;
}

std::wstring SerializeToText(const EditorDocument& doc)
{
    std::wstring out;

    out += L"[song]\r\n";
    out += L"title = " + doc.title + L"\r\n";
    out += L"bpm = " + FormatDouble(doc.bpm) + L"\r\n";
    out += L"time_signature = " + std::to_wstring(doc.beatsPerBar) + L"/" + std::to_wstring(doc.timeSignatureDenominator) + L"\r\n";
    out += L"start_tolerance_ms = " + FormatDouble(doc.startToleranceMs) + L"\r\n";
    out += L"release_tolerance_ms = " + FormatDouble(doc.releaseToleranceMs) + L"\r\n";
    out += L"\r\n";

    for (const EditorClip& clip : doc.clips)
    {
        out += L"[clip]\r\n";
        out += L"display_name = " + clip.displayName + L"\r\n";
        out += L"name = " + clip.name + L"\r\n";
        if (clip.hitsRequired != 16)
        {
            out += L"hits_required = " + std::to_wstring(clip.hitsRequired) + L"\r\n";
        }
        if (clip.startToleranceMs.isOverridden)
        {
            out += L"start_tolerance_ms = " + FormatDouble(clip.startToleranceMs.value) + L"\r\n";
        }
        if (clip.releaseToleranceMs.isOverridden)
        {
            out += L"release_tolerance_ms = " + FormatDouble(clip.releaseToleranceMs.value) + L"\r\n";
        }
        if (clip.initVolume != 1.0)
        {
            out += L"init_volume = " + FormatDouble(clip.initVolume) + L"\r\n";
        }
        if (clip.volume != 1.0)
        {
            out += L"volume = " + FormatDouble(clip.volume) + L"\r\n";
        }
        out += L"\r\n";
    }

    for (const EditorSection& section : doc.sections)
    {
        out += L"[" + std::wstring(SectionKindHeader(section.kind)) + L"]\r\n";
        if (section.kind != SectionKind::Reset)
        {
            const EditorClip* clip = FindClipById(doc, section.clipId);
            // Left empty (rather than skipped) when no clip has been
            // picked yet, so ValidateDocument surfaces the real
            // "[kind] requires a non-empty 'clip'" error instead of a
            // confusing generic parse failure.
            out += L"clip = " + (clip ? clip->name : std::wstring()) + L"\r\n";
            if (section.loopCount != 1)
            {
                out += L"loop_count = " + std::to_wstring(section.loopCount) + L"\r\n";
            }
        }
        out += L"\r\n";
    }

    return out;
}

bool ValidateDocument(const EditorDocument& doc, std::vector<std::wstring>& outErrors)
{
    outErrors.clear();

    if (doc.folderPath.empty())
    {
        outErrors.push_back(L"No song folder yet - use New or Open first.");
        return false;
    }

    std::wstring tempPath = doc.folderPath + L"__rhythmeditor_validate.tmp.chart";
    {
        std::wofstream tempFile(tempPath.c_str(), std::ios::binary);
        if (!tempFile)
        {
            outErrors.push_back(L"Could not write a temporary validation file in " + doc.folderPath);
            return false;
        }
        tempFile << SerializeToText(doc);
    }

    ChartSong tempSong;
    bool ok = ChartFile::Load(tempPath, tempSong, outErrors);
    DeleteFileW(tempPath.c_str());
    return ok;
}

bool SaveDocument(EditorDocument& doc, std::vector<std::wstring>& outErrors)
{
    if (!ValidateDocument(doc, outErrors))
    {
        return false;
    }

    std::wofstream file(doc.chartFilePath.c_str(), std::ios::binary);
    if (!file)
    {
        outErrors.clear();
        outErrors.push_back(L"Could not write " + doc.chartFilePath);
        return false;
    }
    file << SerializeToText(doc);
    file.close();

    doc.dirty = false;
    return true;
}

bool SaveDocumentAs(EditorDocument& doc, const std::wstring& newChartFilePath, std::vector<std::wstring>& outErrors)
{
    outErrors.clear();

    std::wstring newFolder = GetDirectory(newChartFilePath);
    CreateDirectoryW(newFolder.c_str(), nullptr); // best-effort; fine if it already exists

    for (const EditorClip& clip : doc.clips)
    {
        if (clip.hasWav)
        {
            std::wstring src = doc.folderPath + clip.name + L".wav";
            std::wstring dst = newFolder + clip.name + L".wav";
            if (src != dst && !CopyFileW(src.c_str(), dst.c_str(), FALSE))
            {
                outErrors.push_back(L"Could not copy " + src + L" to " + dst);
                return false;
            }
        }
        if (clip.hasMidi)
        {
            std::wstring src = doc.folderPath + clip.name + L".mid";
            std::wstring dst = newFolder + clip.name + L".mid";
            if (src != dst && !CopyFileW(src.c_str(), dst.c_str(), FALSE))
            {
                outErrors.push_back(L"Could not copy " + src + L" to " + dst);
                return false;
            }
        }
    }

    std::wstring oldChartFilePath = doc.chartFilePath;
    std::wstring oldFolderPath = doc.folderPath;
    doc.chartFilePath = newChartFilePath;
    doc.folderPath = newFolder;

    if (!SaveDocument(doc, outErrors))
    {
        doc.chartFilePath = oldChartFilePath;
        doc.folderPath = oldFolderPath;
        return false;
    }
    return true;
}

} // namespace EditorChartIO
