#pragma once

#include <string>
#include <vector>

#include "ChartSong.h"    // SectionKind / LearnMode enums used by value in EditorBlock / EditorClip.
#include "ChartMidi.h"    // LaneNote stored by value in EditorClip::laneNotes.
#include "LaneConfig.h"   // c_LaneCount sizes EditorClip::laneNotes.

// The editor's in-memory representation of a chart being assembled - a separate type from the
// runtime ChartSong/ChartClip/ChartSection, which are produced by parsing an already-valid chart
// and lose information the editor needs while a chart is still being built: whether an optional
// field was explicitly set vs. inherited, and stable identities for clips/blocks that survive edits
// (a ChartSection resolves to a clip index, which shifts on reorder/delete; EditorBlock points at
// an EditorClip::id that never changes).
//
// Vocabulary: the .chart format and runtime call a [learn]/[break]/[reset]/[background] entry a
// "section"; the editor's UI presents these as "blocks" on a horizontal timeline. Presentation
// only, not a different concept.

// A field that may be explicitly set on a clip or left to inherit its song-level default. Mirrors
// the "was this key present" bit ChartSong::Load tracks internally but doesn't expose.
template <typename T>
struct Overridable
{
    bool isOverridden = false;
    T value{};
};

// The editor's in-progress view of one clip - a superset of ChartClip, since it also tracks what a
// resolved ChartClip throws away (whether a tolerance was explicitly set, whether assets exist yet).
struct EditorClip
{
    // Stable editor-only identity, assigned once and never reused. EditorBlock and UI selection
    // reference clips by this, not by position in EditorDocument::clips, so reorder/delete never
    // repoints a block.
    int id = 0;

    // Mirrors ChartClip::name/displayName - see ChartClip.h (name is the cross-reference key and,
    // once saved, the file stem for this clip's .wav/.mid).
    std::wstring name;
    std::wstring displayName;

    // True once <folderPath>\<name>.wav / .mid exist on disk - cached here instead of re-stat'ing
    // every frame. Maintained by ClipPanel's import/rename flows and EditorChartIO::LoadIntoDocument.
    bool hasWav = false;
    bool hasMidi = false;

    // Informational cache from the most recent ChartMidi::LoadLaneNotes call, shown in the clip
    // inspector. NOT authoritative - the real bar-aligned lane notes come from ChartSong::Load
    // re-parsing the .mid at validate/save time, so this can be stale/absent without blocking anything.
    std::vector<LaneNote> laneNotes[c_LaneCount];
    double spanBeats = 4.0;

    int hitsRequired = 16;
    // Declared per-clip only (no song-level default) - see ChartClip.h's LearnMode. Only meaningful
    // for a clip used in a [learn] section.
    LearnMode learnMode = LearnMode::Pass;
    Overridable<double> startToleranceMs;
    Overridable<double> releaseToleranceMs;
    double initVolume = 1.0;
    double volume = 1.0;
};

// The editor's in-progress view of one ChartSection - what the timeline UI calls a "block".
struct EditorBlock
{
    int id = 0;
    SectionKind kind = SectionKind::Learn;

    // References EditorClip::id, or -1 for Reset or a kind with no clip picked yet. Never a raw
    // index into EditorDocument::clips.
    int clipId = -1;

    // Meaningless for Reset - BlockPropertiesPanel never shows a control for it there.
    int loopCount = 1;
};

// The whole chart being assembled: song-level fields, every clip, every block in gameplay order,
// and the bookkeeping the editor tracks edits and undo/redo against.
struct EditorDocument
{
    // Empty only for a brand-new, never-saved document.
    std::wstring chartFilePath;
    // Directory containing chartFilePath and every clip's .wav/.mid - paths are always derived as
    // folderPath + name + extension, matching ChartSong.cpp.
    std::wstring folderPath;

    std::wstring title;
    double bpm = 120.0;
    // The only part of time_signature with runtime effect (ChartSong::Load discards the denominator).
    int beatsPerBar = 4;
    // Cosmetic-only - not stored on ChartSong, so LoadIntoDocument recovers it with a raw-text scan.
    int timeSignatureDenominator = 4;

    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;

    std::vector<EditorClip> clips;
    // Declaration order is gameplay order - the sequence SerializeToText emits as section blocks,
    // after every [clip] block.
    std::vector<EditorBlock> blocks;

    int nextClipId = 1;
    int nextBlockId = 1;

    // True once the document differs from disk (or is brand new). Cleared by a successful Load/Save/SaveAs.
    bool dirty = false;

    // Bumped on every edit. EditorApp compares it against a "last validated" snapshot to decide
    // when debounced live validation re-runs; an edit path just needs to increment it.
    int docVersion = 0;
};

// Finds a clip by its stable id, or nullptr if it no longer exists. Non-const/const overloads so
// callers editing the document don't need a const_cast.
inline EditorClip* FindClipById(EditorDocument& doc, int clipId)
{
    for (EditorClip& clip : doc.clips)
    {
        if (clip.id == clipId)
        {
            return &clip;
        }
    }
    return nullptr;
}

inline const EditorClip* FindClipById(const EditorDocument& doc, int clipId)
{
    for (const EditorClip& clip : doc.clips)
    {
        if (clip.id == clipId)
        {
            return &clip;
        }
    }
    return nullptr;
}

// Marks doc as edited - call after any field change. Sets dirty (the unsaved-changes guard and
// title bar "*") and bumps docVersion (EditorApp's debounced live-validation timer).
inline void MarkDirty(EditorDocument& doc)
{
    doc.dirty = true;
    ++doc.docVersion;
}
