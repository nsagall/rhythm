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
// BlockSchedule::Entry, so the playhead literally cannot dwell there).
class BlockTimeline
{
public:
    // Draws the "Add Block" toolbar and the horizontally-scrolling strip.
    // player may have no schedule yet (validation currently failing) -
    // blocks still draw (from doc.blocks), just with default widths and no
    // playhead. Mutates doc directly (via MarkDirty) on add/reorder - not
    // const.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    int SelectedBlockId() const
    {
        return m_selectedBlockId;
    }

private:
    // On-screen geometry for one block, computed once per frame and shared
    // between the block row and the playhead so they can never disagree.
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

    int m_selectedBlockId = -1;
};
