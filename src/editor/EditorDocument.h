#pragma once

#include <string>
#include <vector>

#include "ChartFile.h"
#include "ChartMidi.h"
#include "LaneConfig.h"

// The editor's own in-memory representation of a chart being assembled -
// deliberately a separate type from ChartSong/ChartClip/ChartSection
// (ChartFile.h), which are the *runtime* structs produced by parsing an
// already-valid chart. Those lose information the editor needs while a
// chart is still being built: whether an optional field was explicitly set
// versus left to inherit a song-level default, and stable identities for
// clips/blocks that survive edits like a renamed clip (ChartSection
// resolves to a plain clip index, which shifts if clips are reordered or
// deleted - EditorBlock instead points at an EditorClip::id that never
// changes for that clip's lifetime).
//
// Note on vocabulary: the .chart file format and the shared runtime
// (ChartFile.h/GameSession.h) call a [learn]/[break]/[reset]/[background]
// entry a "section" - that term is kept everywhere it mirrors the file
// format or the live game. The editor's own UI presents these as "blocks"
// on a horizontal timeline instead (see src/editor/BlockTimeline.h), so
// EditorBlock and EditorDocument::blocks use that name instead - purely a
// presentation-layer distinction, not a different concept.

// A field that may either be explicitly set on a clip or left to inherit
// its song-level default. Mirrors the "was this key present" bit
// ChartFile::Load itself tracks internally while resolving a clip's
// tolerances, but doesn't expose - only the resolved value survives into
// ChartSong/ChartClip.
template <typename T>
struct Overridable
{
    bool isOverridden = false;
    T value{};
};

struct EditorClip
{
    // Stable editor-only identity, assigned once at creation (or on load,
    // one per clip in the source chart) and never reused - EditorBlock
    // and UI selection state reference clips by this, not by their
    // position in EditorDocument::clips, so reordering/deleting other
    // clips never silently repoints a block at the wrong one.
    int id = 0;

    // Mirrors ChartClip::name/displayName - see ChartFile.h for the full
    // semantics (name is both the cross-reference key and, once saved, the
    // literal file stem for this clip's .wav/.mid).
    std::wstring name;
    std::wstring displayName;

    // True once <folderPath>\<name>.wav / .mid exist on disk - kept here
    // instead of re-stat'ing the filesystem every frame. Set by
    // ClipPanel's import flow, cleared by its rename-on-disk "just relabel"
    // path, and refreshed by EditorChartIO::LoadIntoDocument.
    bool hasWav = false;
    bool hasMidi = false;

    // Read-only informational cache from the most recent successful
    // ChartMidi::LoadLaneNotes call (on import, or on load from an
    // already-valid chart) - shown in the clip inspector as a note
    // count/span readout. NOT the authoritative source of truth for what
    // gets saved; the real, bar-aligned lane notes only ever come from
    // ChartFile::Load re-parsing the actual .mid file at validate/save
    // time, so this can be stale/absent without blocking anything.
    std::vector<LaneNote> laneNotes[kLaneCount];
    double spanBeats = 4.0;

    int hitsRequired = 16;
    Overridable<double> startToleranceMs;
    Overridable<double> releaseToleranceMs;
    double initVolume = 1.0;
    double volume = 1.0;
};

struct EditorBlock
{
    int id = 0;
    SectionKind kind = SectionKind::Learn;

    // References EditorClip::id, or -1 for Reset (always) or for any other
    // kind that hasn't had a clip picked yet. Never a raw index into
    // EditorDocument::clips.
    int clipId = -1;

    // Meaningless for Reset - BlockPropertiesPanel never shows a control
    // for it on a Reset block, so it can never be set to anything but this
    // default.
    int loopCount = 1;
};

struct EditorDocument
{
    // Empty only for a brand-new, never-yet-saved document.
    std::wstring chartFilePath;
    // Directory containing chartFilePath, and every clip's .wav/.mid -
    // paths are always derived as folderPath + name + extension, never
    // stored independently (matching how ChartFile.h derives them).
    std::wstring folderPath;

    std::wstring title;
    double bpm = 120.0;
    // The only part of time_signature with runtime effect (ChartFile::Load
    // validates the denominator but discards it).
    int beatsPerBar = 4;
    // Cosmetic-only - not stored on ChartSong at all, so LoadIntoDocument
    // recovers it with a small independent raw-text scan; defaults to 4 on
    // a brand-new document.
    int timeSignatureDenominator = 4;

    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;

    std::vector<EditorClip> clips;
    // Declaration order here is the actual gameplay order - this is the
    // sequence SerializeToText emits as [learn]/[break]/[reset]/
    // [background] blocks, after every [clip] block regardless of how
    // clips and blocks were interleaved while editing.
    std::vector<EditorBlock> blocks;

    int nextClipId = 1;
    int nextBlockId = 1;

    // True once the document differs from what's on disk (or is brand new
    // and has never been saved). Cleared by a successful Load/Save/SaveAs.
    bool dirty = false;

    // Bumped on every edit to any field above. EditorApp compares this
    // against a "last validated" snapshot to decide when debounced live
    // validation should re-run (see EditorChartIO::ValidateDocument) -
    // never read for any other purpose, so any edit path just needs to
    // increment it, not reason about what it means.
    int docVersion = 0;
};

// Finds a clip by its stable id, or nullptr if it no longer exists (e.g.
// referenced by a block whose clip was since deleted). Non-const/const
// overloads so callers editing the document don't need a const_cast.
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

// Marks doc as having been edited - call after any change to any field.
// Sets dirty (drives the unsaved-changes guard and title bar's "*") and
// bumps docVersion (drives EditorApp's debounced live-validation timer).
inline void MarkDirty(EditorDocument& doc)
{
    doc.dirty = true;
    ++doc.docVersion;
}
