#pragma once

#include <string>
#include <vector>

#include "EditorDocument.h"

// All disk I/O and .chart <-> EditorDocument conversion for the editor.
// Kept separate from EditorDocument.h so the plain-data document model has
// no I/O dependencies of its own.
namespace EditorChartIO
{
    // Parses chartFilePath via the real ChartSong::Load and, only if that
    // succeeds, converts the result into outDoc. Returns false (with
    // outErrors describing every problem, exactly as ChartSong::Load would
    // report them) if the chart doesn't already validate cleanly - the
    // editor intentionally does not attempt to load a partially-broken
    // chart, since that would need a second, error-tolerant reimplementation
    // of the whole grammar. A chart broken enough to fail validation must be
    // hand-fixed (or recreated) before the editor can open it.
    bool LoadIntoDocument(const std::wstring& chartFilePath, EditorDocument& outDoc, std::vector<std::wstring>& outErrors);

    // Builds canonical .chart text for doc: one [song] block, then every
    // [clip] block (in doc.clips order), then every section block (in
    // doc.blocks order - the actual gameplay order). Clips are always
    // emitted before sections regardless of how they were interleaved
    // while editing, which is what protects against the format's "a
    // section can only reference a clip declared earlier in the file"
    // constraint. Optional fields are omitted when they equal their
    // documented default, matching the style already used by hand-authored
    // charts under Content/.
    std::wstring SerializeToText(const EditorDocument& doc);

    // Writes SerializeToText(doc) to a throwaway file inside doc.folderPath
    // itself (not system temp - ChartSong::Load derives every clip's
    // wav/mid path from the chart file's own directory) and runs it
    // through the real ChartSong::Load, deleting the temp file afterward
    // either way. This is the sole validation strategy - no rule is
    // reimplemented here, so the editor can never produce a chart the game
    // itself would reject, and stays correct automatically as
    // ChartSong.cpp evolves. Requires doc.folderPath to already exist. If
    // outSong is non-null and validation succeeds, it's filled with the
    // real, already-parsed ChartSong (not yet ExpandLaneNotesToFillClip'd -
    // that needs real stem durations this function doesn't have) - used by
    // the editor's analytical block scheduler (src/editor/BlockSchedule.h)
    // to get a fully-resolved song without a second round-trip through
    // ChartSong::Load.
    bool ValidateDocument(const EditorDocument& doc, std::vector<std::wstring>& outErrors, ChartSong* outSong = nullptr);

    // Validates doc first; on failure, doc.chartFilePath is never touched
    // and outErrors explains why. On success, writes the real file and
    // clears doc.dirty.
    bool SaveDocument(EditorDocument& doc, std::vector<std::wstring>& outErrors);

    // Copies every clip's existing .wav/.mid from doc.folderPath into
    // newChartFilePath's directory (creating it if needed) - required
    // because a clip's asset paths are always derived from the chart's own
    // directory, so simply writing text to a new location would otherwise
    // leave the new chart pointing at files that don't exist there. Only
    // once every copy succeeds does it repoint doc at the new path and run
    // the normal validate-then-write flow. On any failure, doc is left
    // exactly as it was.
    bool SaveDocumentAs(EditorDocument& doc, const std::wstring& newChartFilePath, std::vector<std::wstring>& outErrors);
}
