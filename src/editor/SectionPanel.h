#pragma once

#include "EditorDocument.h"
#include "PreviewPlayer.h"

// The ordered block/section sequence UI - one row per EditorSection, in
// doc.sections order (that order *is* the gameplay order). Add/delete/
// reorder + kind-specific fields (a Reset row exposes neither a clip
// dropdown nor a loop_count field, since the format has no use for either
// there).
class SectionPanel
{
public:
    // previewPlayer may be null (e.g. before a document is loaded); each
    // row's Seek control is simply disabled in that case. Mutates doc
    // directly (via MarkDirty) on any edit - not const.
    void Draw(EditorDocument& doc, PreviewPlayer* previewPlayer);

private:
    // Returns true if this row deleted itself (caller should not advance
    // its loop index, since doc.sections has shifted).
    bool DrawRow(EditorDocument& doc, PreviewPlayer* previewPlayer, size_t index);
};
