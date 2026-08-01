#pragma once

#include "BlockPlayer.h"
#include "EditorDocument.h"

// The upper-right inspector for whichever block is currently selected in
// BlockTimeline - shows/edits the same fields the old SectionPanel edited
// inline per row (kind, clip, loop_count), plus a read-only readout of the
// matching BlockSchedule::Entry's computed timing when one is available,
// so loop_count/hits_required's effect on the schedule is directly
// visible. Also owns the "Delete Block" and "Seek Here" actions for the
// current selection.
class BlockPropertiesPanel
{
public:
    // selectedBlockId may not match any block in doc.blocks (nothing
    // selected, or the selection was deleted elsewhere) - shows a
    // placeholder in that case. player's schedule may not have an entry
    // for this block yet (validation currently failing, or it's a
    // Background/Reset block with no entry of its own - see
    // BlockSchedule::Entry's own comment). Mutates doc directly (via
    // MarkDirty) on edit, and player's playback position via "Seek Here" -
    // not const. Returns true if the selected block was deleted this frame
    // (caller should clear its own selection state).
    bool Draw(EditorDocument& doc, int selectedBlockId, BlockPlayer& player);
};
