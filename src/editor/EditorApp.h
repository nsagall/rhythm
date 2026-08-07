#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "BlockPlayer.h"
#include "BlockPropertiesPanel.h"
#include "BlockTimeline.h"
#include "ClipPanel.h"
#include "EditorDocument.h"
#include "EditorSettings.h"
#include "EditorUndoHistory.h"
#include "SongPropertiesPanel.h"

// Top-level orchestrator: owns the audio engine, block player, the
// currently-open document, its settings, and every panel; lays out the
// fixed (non-docking) window regions each frame and handles every File
// command (New/Open/Save/Save As/Exit), including the unsaved-changes
// guard and error modals.
class EditorApp
{
public:
    // Establishes COM (STA - see the ordering comment in EditorApp.cpp),
    // initializes audio, and reopens the last session's chart if any.
    // Audio failing to initialize is a warning, not fatal - the editor is
    // still useful for assembling a chart without hearing it.
    bool Initialize(HWND hwnd);
    void Shutdown();

    // Advances preview/validation state and draws every panel for this
    // frame. Call once per frame between ImGui::NewFrame() and Render().
    void Update();

    // Called from the window proc's WM_CLOSE handler. Requests an exit
    // through the same unsaved-changes guard as New/Open - if the document
    // is dirty this opens a confirmation modal instead of quitting
    // immediately; WantsToQuit() only becomes true once that's resolved
    // (or immediately, if there was nothing to save).
    void RequestQuit();
    bool WantsToQuit() const
    {
        return m_wantsToQuit;
    }

private:
    enum class PendingAction
    {
        None,
        New,
        Open,
        Exit,
    };

    void DrawMenuBar();
    void DrawSongPropertiesWindow(float x, float y, float w, float h);
    void DrawClipsWindow(float x, float y, float w, float h);
    void DrawBlockPropertiesWindow(float x, float y, float w, float h);
    void DrawBlockTimelineWindow(float x, float y, float w, float h);
    void DrawBottomWindow(float x, float y, float w, float h);
    void DrawDirtyGuardModal();
    void DrawErrorModal();
    void ApplyWindowTitleIfChanged();

    // Persists the current pane splitter positions (see the layout block
    // in Update()) to EditorSettings - called once a drag ends, not every
    // frame while dragging, so resizing a pane doesn't hit disk per-frame.
    void SaveLayoutSettings();

    void DoNew();
    void DoOpen();
    void DoSave();
    void DoSaveAs();
    void DoUndo();
    void DoRedo();

    // Re-resolves the block player's schedule from the current document -
    // called after every successful Load/New/SaveAs, and from Update()'s
    // own debounced revalidation pass once the document settles. Only
    // meaningful while the player is stopped (see BlockPlayer::RebuildSchedule).
    void RebuildBlockSchedule();

    // Mirrors the block timeline's primary selection into the clip panel
    // (selecting a block also selects its clip) - called once per frame
    // after both DrawBlockPropertiesWindow and DrawBlockTimelineWindow
    // have run, so it sees whichever one changed the selection this frame.
    // Only acts when the selection actually changed, so it never fights an
    // independent click made directly in the clip panel.
    void SyncClipSelectionFromBlock();

    // If the current document is dirty, defers action behind the
    // unsaved-changes modal; otherwise runs it immediately.
    void RequestActionWithGuard(PendingAction action);
    void RunPendingAction();

    void ShowErrorModal(const std::string& title, const std::vector<std::wstring>& messages);

    HWND m_hwnd = nullptr;
    bool m_comInitialized = false;

    AudioEngine m_audioEngine;
    BlockPlayer m_blockPlayer{m_audioEngine};
    EditorSettings m_settings;

    SongPropertiesPanel m_songPropertiesPanel;
    ClipPanel m_clipPanel;
    BlockTimeline m_blockTimeline;
    BlockPropertiesPanel m_blockPropertiesPanel;

    EditorDocument m_doc;
    bool m_hasDocument = false;
    EditorUndoHistory m_undoHistory;

    // Last block-timeline primary selection SyncClipSelectionFromBlock has
    // already mirrored into the clip panel - distinct from any real block
    // id (including -1, "nothing selected") so the very first frame after
    // a document loads still performs the initial sync.
    int m_lastSyncedBlockSelectionId = -2;

    // Resizable-pane layout (see Update()). All in pixels, loaded from/
    // saved to EditorSettings so the arrangement survives a restart.
    // m_leftColumnWidth <= 0 is a sentinel meaning "never saved yet" -
    // Update() derives a default (40% of window width) from it once
    // io.DisplaySize is known, which it isn't yet during Initialize().
    float m_leftColumnWidth = -1.0f;
    float m_songPaneHeight = 180.0f;
    float m_timelineHeight = 220.0f;
    float m_bottomHeight = 160.0f;
    // Set while any splitter has moved since the last save; drained (and
    // the layout saved) once the mouse button that was dragging it is
    // released - see Update().
    bool m_layoutDirty = false;

    PendingAction m_pendingAction = PendingAction::None;
    bool m_showDirtyGuardModal = false;

    bool m_showErrorModalFlag = false;
    std::string m_errorModalTitle;
    std::vector<std::wstring> m_errorModalMessages;

    // Debounced live validation - see EditorApp.cpp's Update(). Also drives
    // when RebuildBlockSchedule() re-runs, since the schedule needs the
    // same "settled" document a validation pass does.
    std::vector<std::wstring> m_currentErrors;
    int m_observedVersion = -1;
    int m_lastValidatedVersion = -2;
    unsigned long long m_lastEditTimeMs = 0;

    bool m_wantsToQuit = false;
    std::wstring m_lastAppliedTitle;
};
