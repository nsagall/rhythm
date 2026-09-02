#pragma once

#include <vector>

#include "EditorDocument.h"  // EditorDocument stored by value in m_snapshots and copied inline.

// Snapshot-based undo/redo over EditorDocument: a vector of full document copies plus a cursor.
// Continuous edits (a slider drag, typing) coalesce into one undo step while ImGui reports a widget
// active - see RecordFrame.
class EditorUndoHistory
{
public:
    // Clears all history and records doc as the sole starting point - call after New/Open/startup
    // reload.
    void Reset(const EditorDocument& doc)
    {
        m_snapshots.clear();
        m_snapshots.push_back(doc);
        m_cursor = 0;
        m_activeGestureHasEdit = false;
    }

    // Call once per frame after every panel has had a chance to mutate doc, passing
    // ImGui::IsAnyItemActive() for this frame. If doc changed (via docVersion), starts a new undo
    // step or folds the change into an in-progress gesture's step: the first edit of an unbroken
    // active streak creates a fresh step, later edits in the same streak merge.
    void RecordFrame(const EditorDocument& doc, bool anyItemActive)
    {
        if (doc.docVersion != m_snapshots[m_cursor].docVersion)
        {
            if (!m_activeGestureHasEdit)
            {
                // First edit of the streak - drop any redo-able future and append the new state.
                m_snapshots.erase(m_snapshots.begin() + static_cast<long>(m_cursor) + 1, m_snapshots.end());
                m_snapshots.push_back(doc);
                m_cursor = m_snapshots.size() - 1;
                m_activeGestureHasEdit = true;
            }
            else
            {
                // A later edit in the same streak - keep the current entry in sync, no new step.
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

    // Restores the previous/next snapshot into doc. No-op if CanUndo()/CanRedo() is false.
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

    // True once an edit has been recorded during the current unbroken ImGui-active streak.
    bool m_activeGestureHasEdit = false;
};
