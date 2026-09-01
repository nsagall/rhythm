#pragma once

class BlockPlayer;
struct EditorDocument;

// The upper-right inspector for the block selected in BlockTimeline - shows/edits kind, clip, and
// loop_count, plus a read-only readout of the matching BlockSchedule::Entry's computed timing when
// available. Also owns the "Delete Block", "Duplicate Block", and "Seek Here" actions.
class BlockPropertiesPanel
{
public:
    // Draws the panel and returns the block id that should be selected after this frame.
    //   doc            - the document; mutated on edit.
    //   selectedBlockId - the currently selected block; may match nothing (shows a placeholder).
    //   player         - its schedule may lack an entry for this block; mutated by "Seek Here".
    // Returns selectedBlockId unchanged normally, -1 if the selection was just deleted, or the new
    // id if it was just duplicated - feed straight back into BlockTimeline::SetSelectedBlockId.
    int Draw(EditorDocument& doc, int selectedBlockId, BlockPlayer& player);
};
