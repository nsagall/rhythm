#pragma once

#include <set>
#include <vector>

#include "BlockPlayer.h"
#include "EditorDocument.h"

// The horizontal, drag-and-drop-reorderable block timeline UI - replaces
// the old vertical SectionPanel. Blocks flow left-to-right in doc.blocks
// order (the actual gameplay order); clicking one selects it (read via
// SelectedBlockId() for the single "primary" selection that
// BlockPropertiesPanel elsewhere in the window shows/edits, or
// MultiSelectedBlockIds() for the full set highlighted on the timeline -
// Ctrl+Click toggles a block in or out of that set, Shift+Click selects
// the contiguous range from a fixed anchor to the clicked block, and a
// plain click collapses back to just that one block, all standard
// file-manager conventions). Ctrl+C/Ctrl+V copy/paste whichever blocks are
// selected as a group, inserted right after the current selection's last
// (highest doc.blocks index) member. The Delete key deletes whichever
// blocks are selected, and right-clicking a block opens a context menu with
// the same delete action (selecting just that block first if it wasn't
// already part of the current selection). A playhead shows exactly where
// player's current position falls, via BlockSchedule::Seek - including
// crossing back and re-sweeping a block whose loop_count > 1, and jumping
// instantly across a Background/Reset marker (neither ever gets its own
// BlockSchedule::Entry, so the playhead literally cannot dwell there). A
// thin ruler strip above the blocks (where the playhead marker itself is
// drawn) is click/drag-to-seek - the general "jump playback to a specific
// point" control; BlockPropertiesPanel's own "Seek Here" button is the
// other way in, for jumping to a specific block by name instead of by eye.
class BlockTimeline
{
public:
    // Draws the "Add Block" toolbar and the horizontally-scrolling strip.
    // player may have no schedule yet (validation currently failing) -
    // blocks still draw (from doc.blocks), just with default widths, no
    // playhead, and seeking disabled. Mutates doc directly (via MarkDirty)
    // on add/reorder/delete/paste, and player's playback position via the
    // seek ruler - not const.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    // The single "primary" selection - the one block BlockPropertiesPanel
    // shows/edits, and what its own Duplicate Block/Delete Block buttons
    // act on. Always a member of MultiSelectedBlockIds() (or both empty).
    int SelectedBlockId() const
    {
        return m_selectedBlockId;
    }

    // The full set of blocks highlighted on the timeline right now - what
    // Delete/Copy act on. A single click leaves this as a one-element set
    // equal to SelectedBlockId(); Ctrl/Shift+Click can grow it further.
    const std::set<int>& MultiSelectedBlockIds() const
    {
        return m_multiSelectedBlockIds;
    }

    // Lets BlockPropertiesPanel's own actions (Delete Block, Duplicate
    // Block) update the selection this class owns - e.g. so a freshly
    // duplicated block becomes the new (sole) selection instead of leaving
    // a stale multi-selection highlighted. -1 clears the selection
    // entirely. A no-op if blockId already matches the current primary
    // selection - EditorApp calls this every frame regardless of whether
    // BlockPropertiesPanel actually changed anything, and multi-selection
    // must survive those no-op frames.
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
    // On-screen geometry for one block, computed once per frame and shared
    // between the block row, the playhead, and the seek ruler so they can
    // never disagree.
    struct BlockLayout
    {
        float leftX = 0.0f;
        float width = 0.0f;
        const BlockSchedule::Entry* entry = nullptr; // null for Background/Reset, or if no schedule yet
    };

    void DrawToolbar(EditorDocument& doc);
    // Handles a CLIP_DRAG payload dropped at insertPos (a doc.blocks
    // index, or doc.blocks.size() to append at the end): creates a new
    // block referencing clipId, Learn if the clip has MIDI or Background
    // if it doesn't (a clip with no pattern can't be judged), selects it,
    // and marks the document dirty. No-op if clipId no longer resolves to
    // a clip (e.g. deleted mid-drag).
    void HandleClipDrop(EditorDocument& doc, int clipId, size_t insertPos);
    std::vector<BlockLayout> ComputeLayout(const EditorDocument& doc, const BlockSchedule::Schedule* schedule) const;
    void DrawBlockRow(EditorDocument& doc, const std::vector<BlockLayout>& layout, float originX, float originY);
    void DrawPlayhead(const BlockSchedule::Schedule& schedule, double playheadSeconds,
                       const std::vector<BlockLayout>& layout, float originX, float originY);
    // Draws an invisible click/drag target spanning the ruler strip just
    // above the blocks; while held, converts the mouse's x position back
    // into elapsed seconds (the inverse of DrawPlayhead's position ->
    // x mapping) and calls player.SeekToSeconds. No-op if schedule is
    // null/empty.
    void DrawSeekRuler(BlockPlayer& player, const std::vector<BlockLayout>& layout, float originX, float originY);
    // Inverse of the block-width/phase mapping used to place the playhead:
    // given an x position (relative to the timeline's own origin) and the
    // current layout, returns the elapsed seconds that position
    // corresponds to. A click inside a Learn/Break block's width maps
    // linearly across that block's first loop pass (0..loopSeconds); a
    // click inside a Background/Reset marker (no entry of its own) maps to
    // wherever that block's own instant actually falls, via
    // BlockSchedule's own entries (mirrors BlockPlayer::SeekToBlockStart's
    // fallback, so both land on the same instant for the same block).
    double LayoutXToSeconds(float x, const std::vector<BlockLayout>& layout,
                             const BlockSchedule::Schedule& schedule) const;

    // Click-selection logic shared by every block's InvisibleButton -
    // applies plain/Ctrl/Shift click semantics against clickedId (see the
    // class comment) and updates m_selectedBlockId/m_multiSelectedBlockIds/
    // m_rangeAnchorBlockId accordingly.
    void HandleBlockClick(const EditorDocument& doc, int clickedId);
    // Drops any id from m_multiSelectedBlockIds (and, if needed,
    // m_selectedBlockId/m_rangeAnchorBlockId) that no longer matches a
    // block in doc.blocks - e.g. one just removed by ClipPanel's
    // delete-clip-and-its-blocks cascade, which mutates doc.blocks without
    // going through this class at all. Call once per frame before the
    // selection is read for anything (rendering, Add Block's insert
    // position, keyboard shortcuts).
    void PruneStaleSelection(const EditorDocument& doc);
    // Where Add Block/Paste should insert relative to the current
    // selection: just after the highest-index selected block in
    // doc.blocks, or at the very end if nothing selected (or the selection
    // is stale - see PruneStaleSelection, which runs first every frame).
    size_t InsertPositionAfterSelection(const EditorDocument& doc) const;
    // Erases every block in m_multiSelectedBlockIds from doc.blocks (order-
    // safe: iterates doc.blocks in reverse so earlier erases never
    // invalidate a later index) and clears the selection. No-op if nothing
    // is selected.
    void DeleteSelectedBlocks(EditorDocument& doc);
    // Snapshots every currently-selected block (in doc.blocks order, not
    // click order) into m_clipboard, replacing whatever was copied before.
    // Read-only with respect to doc - never calls MarkDirty, since copying
    // isn't an edit worth being undoable.
    void CopySelectedToClipboard(const EditorDocument& doc);
    // Inserts a fresh-id copy of every block in m_clipboard (in their
    // original relative order) at InsertPositionAfterSelection(), then
    // selects exactly the newly pasted blocks. No-op if the clipboard is
    // empty. The clipboard itself is left intact, so Ctrl+V can be
    // repeated to paste the same blocks again elsewhere.
    void PasteClipboard(EditorDocument& doc);

    int m_selectedBlockId = -1;
    // Always kept in sync with m_selectedBlockId/m_multiSelectedBlockIds -
    // see SetSelectedBlockId and HandleBlockClick. Empty iff nothing is
    // selected.
    std::set<int> m_multiSelectedBlockIds;
    // Fixed reference point for Shift+Click range selection - only moves on
    // a plain or Ctrl+Click, never on a Shift+Click itself, so repeated
    // Shift+Clicks keep extending/contracting from the same original point
    // (standard file-manager behavior) instead of drifting with each click.
    int m_rangeAnchorBlockId = -1;
    // Session-local copy/paste buffer - plain EditorBlock values (with
    // whatever clipId they had; a pasted block still references the same
    // clip, not a duplicate of it), never persisted or touching the real OS
    // clipboard.
    std::vector<EditorBlock> m_clipboard;
};
