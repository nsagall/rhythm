#include "EditorChartIO.h"

#include <windows.h>

#include <cwchar>
#include <fstream>
#include <utility>

#include "ChartSong.h"
#include "ChartTextUtil.h"
#include "EditorDocument.h"

namespace
{

using ChartTextUtil::GetDirectory;

// Formats a double as fixed-point text with trailing zeros trimmed (120.0 -> "120", 87.5 ->
// "87.5"). Never scientific notation, for readability of the saved file.
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
    if (!song.Load(chartFilePath, outErrors))
    {
        return false;
    }

    EditorDocument doc;
    doc.chartFilePath = chartFilePath;
    doc.folderPath = GetDirectory(chartFilePath);
    doc.title = song.Title();
    doc.bpm = song.Bpm();
    doc.beatsPerBar = song.BeatsPerBar();
    doc.timeSignatureDenominator = song.TimeSignatureDenominator();
    doc.startToleranceMs = song.StartToleranceMs();
    doc.releaseToleranceMs = song.ReleaseToleranceMs();

    doc.clips.reserve(song.Clips().size());
    for (const ChartClip& clip : song.Clips())
    {
        EditorClip editorClip;
        editorClip.id = doc.nextClipId++;
        editorClip.name = clip.Name();
        editorClip.displayName = clip.DisplayName();
        // ChartSong::Load fails hard on a missing .wav, so any clip in song.Clips() has one.
        editorClip.hasWav = true;
        editorClip.hasMidi = clip.HasMidi();
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            editorClip.laneNotes[lane] = clip.LaneNotes(lane);
        }
        editorClip.spanBeats = clip.SpanBeats();
        editorClip.hitsRequired = clip.HitsRequired();
        editorClip.learnMode = clip.Mode();
        // "Equals the song default" heuristic: an unset override is bit-identical to the song value
        // here, so an explicit override that happens to match displays as inherited - harmless,
        // since the resolved value is the same either way.
        editorClip.startToleranceMs.isOverridden = (clip.StartToleranceMs() != song.StartToleranceMs());
        editorClip.startToleranceMs.value = clip.StartToleranceMs();
        editorClip.releaseToleranceMs.isOverridden = (clip.ReleaseToleranceMs() != song.ReleaseToleranceMs());
        editorClip.releaseToleranceMs.value = clip.ReleaseToleranceMs();
        editorClip.initVolume = clip.InitVolume();
        editorClip.volume = clip.Volume();
        doc.clips.push_back(std::move(editorClip));
    }

    doc.blocks.reserve(song.Sections().size());
    for (const ChartSection& section : song.Sections())
    {
        EditorBlock editorBlock;
        editorBlock.id = doc.nextBlockId++;
        editorBlock.kind = section.kind;
        editorBlock.clipId = (section.clipIndex >= 0 && static_cast<size_t>(section.clipIndex) < doc.clips.size())
                                  ? doc.clips[static_cast<size_t>(section.clipIndex)].id
                                  : -1;
        editorBlock.loopCount = section.loopCount;
        doc.blocks.push_back(editorBlock);
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
        if (clip.learnMode != LearnMode::Pass)
        {
            out += L"learn_mode = dontfail\r\n";
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

    for (const EditorBlock& block : doc.blocks)
    {
        out += L"[" + std::wstring(SectionKindHeader(block.kind)) + L"]\r\n";
        if (block.kind != SectionKind::Reset)
        {
            const EditorClip* clip = FindClipById(doc, block.clipId);
            // Emitted empty (not skipped) when no clip is picked, so ValidateDocument surfaces the
            // real "[kind] requires a non-empty 'clip'" error rather than a generic parse failure.
            out += L"clip = " + (clip ? clip->name : std::wstring()) + L"\r\n";
            if (block.loopCount != 1)
            {
                out += L"loop_count = " + std::to_wstring(block.loopCount) + L"\r\n";
            }
        }
        out += L"\r\n";
    }

    return out;
}

bool ValidateDocument(const EditorDocument& doc, std::vector<std::wstring>& outErrors, ChartSong* outSong)
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
    bool ok = tempSong.Load(tempPath, outErrors);
    DeleteFileW(tempPath.c_str());
    if (ok && outSong != nullptr)
    {
        *outSong = std::move(tempSong);
    }
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
