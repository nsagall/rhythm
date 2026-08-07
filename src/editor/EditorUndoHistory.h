#pragma once

#include <vector>

#include "EditorDocument.h"

// Snapshot-based undo/redo over EditorDocument. A vector of full document
// copies plus a cursor - m_snapshots[m_cursor] always mirrors the live
// document exactly except mid-Undo()/Redo() call. EditorDocument is a plain
// value type (no pointers/engine handles anywhere in it or EditorClip/
// EditorBlock), so deep-copying it per edit is cheap and safe - this avoids
// having to hand-write an inverse for every one of the editor's mutation
// sites (delete-with-references, multi-clip Detect, drag-reorder, rename,
// etc.), and nextClipId/nextBlockId "just work" across undo/redo since each
// snapshot carries its own consistent values.
//
// Continuous edits (a slider drag, a text field being typed into) are
// coalesced into a single undo step for as long as ImGui reports a widget
// as active, so Ctrl+Z after dragging a volume slider undoes the whole
// drag, not one frame of it - see RecordFrame's own comment. This needs no
// per-widget changes anywhere else in the editor; it works uniformly off
// ImGui's own active-item state.
class EditorUndoHistory
{
public:
    // Clears all history and records doc as the sole starting point - call
    // right after New/Open/the startup reload, where "undo past this"
    // makes no sense.
    void Reset(const EditorDocument& doc)
    {
        m_snapshots.clear();
        m_snapshots.push_back(doc);
        m_cursor = 0;
        m_activeGestureHasEdit = false;
    }

    // Call once per frame, after every panel has had a chance to mutate
    // doc, passing ImGui::IsAnyItemActive() for this same frame. Detects
    // whether doc changed since the last call (via docVersion) and, if so,
    // either starts a new undo step or folds the change into an
    // already-in-progress gesture's existing step (see m_activeGestureHasEdit
    // below for what "in progress" means here).
    //
    // Deliberately NOT keyed directly off "was anyItemActive last frame":
    // that would be correct for a widget whose held/active state and its
    // edits start on the same frame (a slider tick, a keystroke), but wrong
    // for one where holding starts well before any edit does - e.g.
    // BlockTimeline's drag-and-drop reorder, where the source block's
    // InvisibleButton goes active the moment the mouse is pressed, but
    // doc.blocks isn't touched until the drop completes, possibly many
    // frames later; or a plain button click (Delete Block, Duplicate
    // Block, Add Block's popup), whose own MarkDirty happens on the
    // release frame - by which point the button has already cleared its
    // own active state this same frame, but the *previous* frame (the
    // press) was active with no edit yet. Naively merging into whatever
    // snapshot was current when the widget first went active would silently
    // overwrite the pre-edit baseline for exactly these one-shot
    // mutations, making them look like they can't be undone at all.
    //
    // m_activeGestureHasEdit instead tracks "has an edit already been
    // recorded during the current unbroken active streak" - only true once
    // a docVersion change has actually been folded into it, so the first
    // edit of any streak (immediate or delayed) always creates a fresh
    // step, and only a second-or-later edit within the *same* streak
    // merges. It's cleared the moment anyItemActive goes false, so the next
    // edit - whenever it comes - starts fresh again.
    void RecordFrame(const EditorDocument& doc, bool anyItemActive)
    {
        if (doc.docVersion != m_snapshots[m_cursor].docVersion)
        {
            if (!m_activeGestureHasEdit)
            {
                // First edit since the active streak began (if any) - drop
                // any redo-able future (a new edit branches away from it)
                // and append the new current state as its own step.
                m_snapshots.erase(m_snapshots.begin() + static_cast<long>(m_cursor) + 1, m_snapshots.end());
                m_snapshots.push_back(doc);
                m_cursor = m_snapshots.size() - 1;
                m_activeGestureHasEdit = true;
            }
            else
            {
                // A later edit in the same still-active streak - keep the
                // current entry in sync without creating a new undo step.
                m_snapshots[m_cursor] = doc;
            }
        }

        if (!anyItemActive)
        {
            m_activeGestureHasEdit = false;
        }
    }

    bool CanUndo() const
    {
        return m_cursor > 0;
    }
    bool CanRedo() const
    {
        return m_cursor + 1 < m_snapshots.size();
    }

    // Restores the previous/next snapshot into doc. No-op if
    // CanUndo()/CanRedo() is false. Resets the in-progress-gesture state so
    // the restoration itself is never mistaken for a new user edit.
    void Undo(EditorDocument& doc)
    {
        if (!CanUndo())
        {
            return;
        }
        --m_cursor;
        doc = m_snapshots[m_cursor];
        m_activeGestureHasEdit = false;
    }

    void Redo(EditorDocument& doc)
    {
        if (!CanRedo())
        {
            return;
        }
        ++m_cursor;
        doc = m_snapshots[m_cursor];
        m_activeGestureHasEdit = false;
    }

private:
    std::vector<EditorDocument> m_snapshots;
    size_t m_cursor = 0;
    // True once an edit has been recorded during the current unbroken
    // ImGui-active streak - see RecordFrame's own comment for why this,
    // and not just "was anyItemActive last frame", is the correct signal
    // for whether the *next* edit should merge into it.
    bool m_activeGestureHasEdit = false;
};
