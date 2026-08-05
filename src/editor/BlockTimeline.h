#pragma once

#include <vector>

#include "BlockPlayer.h"
#include "EditorDocument.h"

// The horizontal, drag-and-drop-reorderable block timeline UI - replaces
// the old vertical SectionPanel. Blocks flow left-to-right in doc.blocks
// order (the actual gameplay order); clicking one selects it (read via
// SelectedBlockId(), consumed by BlockPropertiesPanel elsewhere in the
// window to show/edit its fields); a playhead shows exactly where
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
    // on add/reorder, and player's playback position via the seek ruler -
    // not const.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    int SelectedBlockId() const
    {
        return m_selectedBlockId;
    }

    // Lets BlockPropertiesPanel's own actions (Delete Block, Duplicate
    // Block) update the selection this class owns - e.g. so a freshly
    // duplicated block becomes the new selection instead of leaving the
    // original selected. -1 clears the selection.
    void SetSelectedBlockId(int blockId)
    {
        m_selectedBlockId = blockId;
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

    int m_selectedBlockId = -1;
};
