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
        m_gestureInProgress = false;
    }

    // Call once per frame, after every panel has had a chance to mutate
    // doc, passing ImGui::IsAnyItemActive() for this same frame. Detects
    // whether doc changed since the last call (via docVersion) and, if so,
    // either starts a new undo step (the previously-recorded state becomes
    // the undo target) or folds the change into the in-progress gesture
    // (anyItemActive stayed true across frames - e.g. still dragging, still
    // typing). Once anyItemActive goes false, the next change starts a
    // fresh step even if it's the same field.
    void RecordFrame(const EditorDocument& doc, bool anyItemActive)
    {
        if (doc.docVersion != m_snapshots[m_cursor].docVersion)
        {
            if (!m_gestureInProgress)
            {
                // A new, separately-undoable edit: drop any redo-able
                // future (a new edit branches away from it) and append the
                // new current state.
                m_snapshots.erase(m_snapshots.begin() + static_cast<long>(m_cursor) + 1, m_snapshots.end());
                m_snapshots.push_back(doc);
                m_cursor = m_snapshots.size() - 1;
            }
            else
            {
                // Same gesture continuing - keep the current entry in sync
                // without creating a new undo step.
                m_snapshots[m_cursor] = doc;
            }
        }
        m_gestureInProgress = anyItemActive;
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
    // CanUndo()/CanRedo() is false. Resets the in-progress-gesture flag so
    // the restoration itself is never mistaken for a new user edit.
    void Undo(EditorDocument& doc)
    {
        if (!CanUndo())
        {
            return;
        }
        --m_cursor;
        doc = m_snapshots[m_cursor];
        m_gestureInProgress = false;
    }

    void Redo(EditorDocument& doc)
    {
        if (!CanRedo())
        {
            return;
        }
        ++m_cursor;
        doc = m_snapshots[m_cursor];
        m_gestureInProgress = false;
    }

private:
    std::vector<EditorDocument> m_snapshots;
    size_t m_cursor = 0;
    bool m_gestureInProgress = false;
};
