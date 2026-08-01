#pragma once

#include <windows.h>

#include <string>

#include "EditorDocument.h"

// Clip library UI: a list of the document's clips on the left, an
// inspector for the selected one on the right. Owns its own selection and
// modal-flow state; DrawList/DrawInspector below share that state, so this
// stays a single panel class rather than two independent ones.
class ClipPanel
{
public:
    // Draws the panel's content into whatever ImGui window/child region the
    // caller has already begun. owner is the editor's HWND, used for
    // native Import Wav/Import Midi file pickers. Mutates doc directly
    // (via MarkDirty) on any edit - not const.
    void Draw(EditorDocument& doc, HWND owner);

    int SelectedClipId() const
    {
        return m_selectedClipId;
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
    // Scans doc.folderPath for .wav/.mid files not already used by any
    // existing clip (matched by base filename against EditorClip::name)
    // and creates a new clip per unmatched .wav - paired with its .mid if
    // one exists alongside it, wav-only otherwise. A .mid with no matching
    // .wav is skipped (a clip always needs audio). Sets m_lastDetectSummary
    // for DrawList to show a one-line result.
    void DetectClips(EditorDocument& doc);

    int m_selectedClipId = -1;

    // Which clip's fields the scratch text buffers below currently reflect
    // (-1 = none loaded yet) - buffers are only re-synced from the document
    // when this stops matching m_selectedClipId, so in-progress typing is
    // never clobbered by re-reading the (unchanged) source value mid-edit.
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

    // One-line result of the most recent Detect click (e.g. "Found 3 new
    // clip(s)."), shown under the toolbar until the next Detect - empty
    // means nothing to show.
    std::wstring m_lastDetectSummary;
};
