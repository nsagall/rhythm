#pragma once

#include <string>

#include "BlockPlayer.h"
#include "EditorDocument.h"

// "3:45" - never plain seconds, so a long song doesn't read as an
// implausible three-digit number. Shared with EditorApp's own "Estimated
// length" readout in the bottom Preview panel, so the two stay worded
// identically.
std::wstring FormatMinutesSeconds(double seconds);

// Editor panel for the chart's song-level fields - the [song] block's own
// values (title, tempo, time signature, default hit tolerances), as
// opposed to per-clip overrides (ClipPanel's tolerance fields) or
// per-block fields (BlockPropertiesPanel). Also surfaces the chart's
// overall estimated length under perfect play (see Draw) - a computed,
// not stored, song-level fact.
class SongPropertiesPanel
{
public:
    // Draws the panel's content into whatever ImGui window the caller has
    // already begun. Mutates doc directly (via MarkDirty) on any edit.
    // player supplies the current BlockSchedule (via CurrentSchedule()) for
    // the estimated-length readout - the same schedule/perfect-play
    // assumption BlockTimeline's playhead and EditorApp's own "Estimated
    // length" readout in the bottom Preview panel are built on (see
    // BlockSchedule.h) - not const.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    // Forces the title edit buffer to re-sync from doc on the next Draw()
    // call instead of showing stale staged text - call after anything that
    // replaces doc's contents out from under this panel (New/Open, or
    // undo/redo restoring a snapshot).
    void NotifyDocumentReplaced()
    {
        m_titleSynced = false;
    }

private:
    bool m_titleSynced = false;
    char m_titleBuf[256] = {};
};
