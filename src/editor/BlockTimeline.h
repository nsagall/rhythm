#pragma once

#include <set>
#include <vector>

#include "BlockPlayer.h"
#include "EditorDocument.h"

// The horizontal, drag-and-drop-reorderable block timeline UI. Blocks flow left-to-right in
// doc.blocks (gameplay) order. Clicking one selects it - SelectedBlockId() is the single "primary"
// selection BlockPropertiesPanel shows/edits, MultiSelectedBlockIds() the full highlighted set
// (Ctrl+Click toggles, Shift+Click ranges from an anchor, plain click collapses; standard
// file-manager conventions). Ctrl+C/Ctrl+V copy/paste the selection as a group after its last
// member; Delete and the right-click menu delete the selection. A playhead tracks the player's
// position via BlockSchedule::Seek, including re-sweeping a loop_count > 1 block and jumping across
// a Background/Reset marker. The ruler strip above the blocks is click/drag-to-seek.
class BlockTimeline
{
public:
    // Draws the "Add Block" toolbar and the horizontally-scrolling strip. player may have no
    // schedule yet (validation failing) - blocks still draw with default widths, no playhead, no
    // seeking. Mutates doc on add/reorder/delete/paste and player's position via the seek ruler.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    // The "primary" selection - the one block BlockPropertiesPanel shows/edits. Always a member of
    // MultiSelectedBlockIds() (or both empty).
    int SelectedBlockId() const
    {
        return m_selectedBlockId;
    }

    // The full set of highlighted blocks - what Delete/Copy act on. A plain click leaves this as a
    // one-element set equal to SelectedBlockId(); Ctrl/Shift+Click grow it.
    const std::set<int>& MultiSelectedBlockIds() const
    {
        return m_multiSelectedBlockIds;
    }

    // Lets BlockPropertiesPanel's actions update the selection this class owns (e.g. so a
    // duplicated block becomes the new sole selection). -1 clears it. A no-op if blockId already
    // matches the primary selection - EditorApp calls this every frame, and multi-selection must
    // survive those no-op frames.
    void SetSelectedBlockId(int blockId)
    {
        if (blockId == m_selectedBlockId)
        {
            return;
        }
        m_selectedBlockId = blockId;
        m_rangeAnchorBlockId = blockId;
        m_multiSelectedBlockIds.clear();
        if (blockId >= 0)
        {
            m_multiSelectedBlockIds.insert(blockId);
        }
    }

private:
    // On-screen geometry for one block, computed once per frame and shared by the block row, the
    // playhead, and the seek ruler so they can't disagree.
    struct BlockLayout
    {
        float leftX = 0.0f;
        float width = 0.0f;
        const BlockSchedule::Entry* entry = nullptr; // null for Background/Reset, or if no schedule yet
    };

    void DrawToolbar(EditorDocument& doc);
    // Handles a CLIP_DRAG payload dropped at insertPos (a doc.blocks index, or size() to append):
    // creates a block referencing clipId - Learn if the clip has MIDI, else Background - selects
    // it, and marks the document dirty. No-op if clipId no longer resolves.
    void HandleClipDrop(EditorDocument& doc, int clipId, size_t insertPos);
    std::vector<BlockLayout> ComputeLayout(const EditorDocument& doc, const BlockSchedule::Schedule* schedule) const;
    void DrawBlockRow(EditorDocument& doc, const std::vector<BlockLayout>& layout, float originX, float originY);
    void DrawPlayhead(const BlockSchedule::Schedule& schedule, double playheadSeconds,
                       const std::vector<BlockLayout>& layout, float originX, float originY);
    // Draws an invisible click/drag target over the ruler strip; while held, converts the mouse x
    // back into elapsed seconds (inverse of DrawPlayhead's mapping) and calls player.SeekToSeconds.
    // No-op if schedule is null/empty.
    void DrawSeekRuler(BlockPlayer& player, const std::vector<BlockLayout>& layout, float originX, float originY);
    // Inverse of the playhead's position -> x mapping: returns the elapsed seconds an x position
    // (relative to the timeline origin) corresponds to. A click in a Learn/Break block maps
    // linearly across its first loop pass; a click in a Background/Reset marker maps to where that
    // block's instant falls (mirrors BlockPlayer::SeekToBlockStart's fallback).
    double LayoutXToSeconds(float x, const std::vector<BlockLayout>& layout,
                             const BlockSchedule::Schedule& schedule) const;

    // Click-selection logic shared by every block's InvisibleButton - applies plain/Ctrl/Shift
    // semantics against clickedId and updates the selection members.
    void HandleBlockClick(const EditorDocument& doc, int clickedId);
    // Drops any selection id that no longer matches a block in doc.blocks - e.g. one removed by
    // ClipPanel's delete-clip cascade, which mutates doc.blocks without going through this class.
    // Call once per frame before the selection is read.
    void PruneStaleSelection(const EditorDocument& doc);
    // Where Add Block/Paste should insert: just after the highest-index selected block, or at the
    // end if nothing is selected.
    size_t InsertPositionAfterSelection(const EditorDocument& doc) const;
    // Erases every block in m_multiSelectedBlockIds from doc.blocks (iterating in reverse so
    // earlier erases don't invalidate a later index) and clears the selection.
    void DeleteSelectedBlocks(EditorDocument& doc);
    // Snapshots every selected block (in doc.blocks order) into m_clipboard, replacing what was
    // there. Never calls MarkDirty - copying isn't an undoable edit.
    void CopySelectedToClipboard(const EditorDocument& doc);
    // Inserts a fresh-id copy of every clipboard block (in original order) at
    // InsertPositionAfterSelection(), then selects the pasted blocks. The clipboard is left intact,
    // so Ctrl+V can repeat.
    void PasteClipboard(EditorDocument& doc);

    int m_selectedBlockId = -1;
    // Always in sync with m_selectedBlockId (see SetSelectedBlockId/HandleBlockClick). Empty iff
    // nothing is selected.
    std::set<int> m_multiSelectedBlockIds;
    // Fixed reference point for Shift+Click range selection - moves only on a plain or Ctrl+Click,
    // so repeated Shift+Clicks extend/contract from the same point.
    int m_rangeAnchorBlockId = -1;
    // Session-local copy/paste buffer - plain EditorBlock values (a pasted block references the
    // same clip, not a copy of it). Never persisted or touching the OS clipboard.
    std::vector<EditorBlock> m_clipboard;
};
