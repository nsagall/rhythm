#pragma once

#include <windows.h>

#include <string>

struct EditorDocument;
struct EditorClip;

// Clip library UI: a list of the document's clips on the left, an inspector for the selected one on
// the right. Owns its own selection and modal-flow state.
class ClipPanel
{
public:
    // Draws the panel into whatever ImGui window/child the caller has begun. owner is the editor's
    // HWND, for native Import Wav/Midi file pickers. Mutates doc on any edit.
    void Draw(EditorDocument& doc, HWND owner);

    int SelectedClipId() const
    {
        return m_selectedClipId;
    }

    // Lets EditorApp mirror the block timeline's selection here, so picking a block also shows its
    // clip. clipId may be -1 (a Reset block, or one with no clip yet), which clears the selection.
    void SetSelectedClipId(int clipId)
    {
        m_selectedClipId = clipId;
    }

    // Forces the Name/Display Name edit buffers to re-sync from doc on the next Draw() - call after
    // anything that replaces doc's contents (undo/redo).
    void NotifyDocumentReplaced()
    {
        m_scratchForClipId = -1;
    }

private:
    void DrawList(EditorDocument& doc);
    void DrawInspector(EditorDocument& doc, HWND owner);
    void SyncScratchIfNeeded(const EditorClip& clip);
    void ImportFile(EditorDocument& doc, EditorClip& clip, bool isMidi, HWND owner);
    void CompleteImport(EditorDocument& doc, EditorClip& clip, const std::wstring& src, const std::wstring& dst, bool isMidi);
    void CommitNameChange(EditorDocument& doc, EditorClip& clip, const std::wstring& newName);
    void DrawRenameModal(EditorDocument& doc);
    void DrawOverwriteModal(EditorDocument& doc);
    void DrawDeleteModal(EditorDocument& doc);
    void CreateNewClip(EditorDocument& doc);
    void DuplicateSelected(EditorDocument& doc);
    void RequestDelete(EditorDocument& doc, int clipId);
    // Scans doc.folderPath for .wav/.mid files not already used by a clip (matched by base filename
    // against EditorClip::name) and creates one clip per unmatched .wav, paired with its .mid if
    // present. A .mid with no matching .wav is skipped. Sets m_lastDetectSummary.
    void DetectClips(EditorDocument& doc);

    int m_selectedClipId = -1;

    // Which clip the scratch text buffers below reflect (-1 = none). Buffers only re-sync from the
    // document when this stops matching m_selectedClipId, so in-progress typing isn't clobbered.
    int m_scratchForClipId = -1;
    char m_nameBuf[256] = {};
    char m_displayNameBuf[256] = {};

    // Rename-on-disk modal state (see CommitNameChange).
    bool m_showRenameModal = false;
    int m_renameClipId = -1;
    std::wstring m_renameOldName;
    std::wstring m_renameNewName;

    // Import overwrite-confirmation modal state (see ImportFile).
    bool m_showOverwriteModal = false;
    int m_overwriteClipId = -1;
    std::wstring m_overwriteSrc;
    std::wstring m_overwriteDst;
    bool m_overwriteIsMidi = false;

    // Delete-with-references modal state (see RequestDelete).
    bool m_showDeleteModal = false;
    int m_deleteClipId = -1;

    std::wstring m_lastMidiImportError;

    // One-line result of the most recent Detect click, shown under the toolbar until the next
    // Detect. Empty means nothing to show.
    std::wstring m_lastDetectSummary;
};
