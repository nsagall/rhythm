#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ClipPanel.h"
#include "EditorDocument.h"
#include "EditorSettings.h"
#include "PreviewPlayer.h"
#include "SectionPanel.h"

// Top-level orchestrator: owns the audio engine, preview player, the
// currently-open document, its settings, and both panels; lays out the
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
    void DrawClipsWindow(float x, float y, float w, float h);
    void DrawSectionsWindow(float x, float y, float w, float h);
    void DrawBottomWindow(float x, float y, float w, float h);
    void DrawDirtyGuardModal();
    void DrawErrorModal();
    void ApplyWindowTitleIfChanged();

    void DoNew();
    void DoOpen();
    void DoSave();
    void DoSaveAs();

    // If the current document is dirty, defers action behind the
    // unsaved-changes modal; otherwise runs it immediately.
    void RequestActionWithGuard(PendingAction action);
    void RunPendingAction();

    void ShowErrorModal(const std::string& title, const std::vector<std::wstring>& messages);

    HWND m_hwnd = nullptr;
    bool m_comInitialized = false;

    AudioEngine m_audioEngine;
    PreviewPlayer m_previewPlayer{m_audioEngine};
    EditorSettings m_settings;

    ClipPanel m_clipPanel;
    SectionPanel m_sectionPanel;

    EditorDocument m_doc;
    bool m_hasDocument = false;

    PendingAction m_pendingAction = PendingAction::None;
    bool m_showDirtyGuardModal = false;

    bool m_showErrorModalFlag = false;
    std::string m_errorModalTitle;
    std::vector<std::wstring> m_errorModalMessages;

    // Debounced live validation - see EditorApp.cpp's Update().
    std::vector<std::wstring> m_currentErrors;
    int m_observedVersion = -1;
    int m_lastValidatedVersion = -2;
    unsigned long long m_lastEditTimeMs = 0;

    bool m_wantsToQuit = false;
    std::wstring m_lastAppliedTitle;
};
