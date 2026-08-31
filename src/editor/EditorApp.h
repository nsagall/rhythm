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

// Top-level orchestrator: owns the audio engine, block player, the open document, its settings, and
// every panel; lays out the window regions each frame and handles every File command, including the
// unsaved-changes guard and error modals.
class EditorApp
{
public:
    // Establishes COM (STA), initializes audio, and reopens the last session's chart if any. Audio
    // failing to initialize is a warning, not fatal.
    bool Initialize(HWND hwnd);
    void Shutdown();

    // Advances preview/validation state and draws every panel. Call once per frame between
    // ImGui::NewFrame() and Render().
    void Update();

    // Called from the WM_CLOSE handler. Requests an exit through the same unsaved-changes guard as
    // New/Open - a dirty document opens a confirmation modal; WantsToQuit() only becomes true once
    // that resolves.
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

    // Persists the current pane splitter positions to EditorSettings - called once a drag ends,
    // not per-frame while dragging.
    void SaveLayoutSettings();

    void DoNew();
    void DoOpen();
    void DoSave();
    void DoSaveAs();
    void DoUndo();
    void DoRedo();

    // Re-resolves the block player's schedule from the current document - called after every
    // Load/New/SaveAs and from Update()'s debounced revalidation once the document settles. Only
    // meaningful while the player is stopped.
    void RebuildBlockSchedule();

    // Mirrors the block timeline's primary selection into the clip panel (selecting a block also
    // selects its clip). Called once per frame after both block panels have drawn. Only acts when
    // the selection changed, so it never fights an independent click in the clip panel.
    void SyncClipSelectionFromBlock();

    // If the document is dirty, defers action behind the unsaved-changes modal; otherwise runs it
    // immediately.
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

    // Last block-timeline selection SyncClipSelectionFromBlock mirrored into the clip panel -
    // distinct from any real block id (including -1) so the first frame after a load still syncs.
    int m_lastSyncedBlockSelectionId = -2;

    // Resizable-pane layout, in pixels, loaded from / saved to EditorSettings. m_leftColumnWidth <= 0
    // means "never saved yet" - Update() then derives a default (40% of window width) once
    // io.DisplaySize is known.
    float m_leftColumnWidth = -1.0f;
    float m_songPaneHeight = 180.0f;
    float m_timelineHeight = 220.0f;
    float m_bottomHeight = 160.0f;
    // Set while a splitter has moved since the last save; drained (and the layout saved) once the
    // drag's mouse button is released.
    bool m_layoutDirty = false;

    PendingAction m_pendingAction = PendingAction::None;
    bool m_showDirtyGuardModal = false;

    bool m_showErrorModalFlag = false;
    std::string m_errorModalTitle;
    std::vector<std::wstring> m_errorModalMessages;

    // Debounced live validation (see Update()). Also drives when RebuildBlockSchedule() re-runs,
    // since it needs the same "settled" document.
    std::vector<std::wstring> m_currentErrors;
    int m_observedVersion = -1;
    int m_lastValidatedVersion = -2;
    unsigned long long m_lastEditTimeMs = 0;

    bool m_wantsToQuit = false;
    std::wstring m_lastAppliedTitle;
};
