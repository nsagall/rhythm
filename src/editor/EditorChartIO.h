#pragma once

#include <string>
#include <vector>

struct EditorDocument;
class ChartSong;

// All disk I/O and .chart <-> EditorDocument conversion for the editor. Kept separate from
// EditorDocument.h so the plain-data document model has no I/O dependencies of its own.
namespace EditorChartIO
{
    // Parses chartFilePath via the real ChartSong::Load and, on success, converts the result into
    // outDoc. Returns false (outErrors as ChartSong::Load would report them) if the chart doesn't
    // already validate cleanly - the editor does not load a partially-broken chart, which would
    // need an error-tolerant reimplementation of the grammar.
    bool LoadIntoDocument(const std::wstring& chartFilePath, EditorDocument& outDoc, std::vector<std::wstring>& outErrors);

    // Builds canonical .chart text for doc: one [song] block, then every [clip] block (doc.clips
    // order), then every section block (doc.blocks / gameplay order). Clips always come before
    // sections, satisfying the format's "a section can only reference an earlier clip" rule.
    // Optional fields at their default are omitted.
    std::wstring SerializeToText(const EditorDocument& doc);

    // Writes SerializeToText(doc) to a throwaway file inside doc.folderPath (not system temp -
    // ChartSong::Load derives clip paths from the chart file's directory) and runs it through the
    // real ChartSong::Load, deleting the temp file afterward. The sole validation strategy - no
    // rule is reimplemented, so the editor can't produce a chart the game would reject.
    //   doc       - the document to validate; doc.folderPath must already exist.
    //   outErrors - filled with every problem found on failure.
    //   outSong   - if non-null and validation succeeds, filled with the parsed ChartSong (not yet
    //               ExpandLaneNotesToFillClip'd), for the editor's block scheduler.
    // Returns whether the document validates.
    bool ValidateDocument(const EditorDocument& doc, std::vector<std::wstring>& outErrors, ChartSong* outSong = nullptr);

    // Validates doc, then writes the real file and clears doc.dirty. On failure, doc.chartFilePath
    // is never touched and outErrors explains why.
    bool SaveDocument(EditorDocument& doc, std::vector<std::wstring>& outErrors);

    // Copies every clip's .wav/.mid from doc.folderPath into newChartFilePath's directory (created
    // if needed), since clip asset paths derive from the chart's directory. Only once every copy
    // succeeds does it repoint doc and run the normal validate-then-write flow. On any failure, doc
    // is left exactly as it was.
    bool SaveDocumentAs(EditorDocument& doc, const std::wstring& newChartFilePath, std::vector<std::wstring>& outErrors);
}
