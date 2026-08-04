#pragma once

#include "EditorDocument.h"

// Editor panel for the chart's song-level fields - the [song] block's own
// values (title, tempo, time signature, default hit tolerances), as
// opposed to per-clip overrides (ClipPanel's tolerance fields) or
// per-block fields (BlockPropertiesPanel).
class SongPropertiesPanel
{
public:
    // Draws the panel's content into whatever ImGui window the caller has
    // already begun. Mutates doc directly (via MarkDirty) on any edit.
    void Draw(EditorDocument& doc);

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
