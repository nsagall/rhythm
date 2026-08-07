#include "EditorApp.h"

#include <objbase.h>

#include <cwchar>

#include <imgui.h>

#include "EditorChartIO.h"
#include "FileDialogs.h"
#include "PaneSplitter.h"
#include "Utf8.h"

namespace
{

std::wstring StripTrailingSlash(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }
    return path;
}

std::wstring LastPathComponent(const std::wstring& path)
{
    std::wstring stripped = StripTrailingSlash(path);
    size_t slash = stripped.find_last_of(L"\\/");
    return slash == std::wstring::npos ? stripped : stripped.substr(slash + 1);
}

} // namespace

bool EditorApp::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;

    // Establish an STA COM apartment before AudioEngine::Initialize() runs.
    // IFileOpenDialog/IFileSaveDialog (FileDialogs.cpp) require STA to
    // behave correctly. AudioEngine::Initialize() makes its own
    // CoInitializeEx(COINIT_MULTITHREADED) attempt internally, but already
    // tolerates finding COM in a different apartment mode - it treats
    // RPC_E_CHANGED_MODE as non-fatal and correctly avoids
    // CoUninitialize()-ing a mode it didn't establish (see
    // AudioEngine.cpp), so this ordering is safe; XAudio2 itself doesn't
    // care which apartment it's created from.
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(comHr);

    if (!m_audioEngine.Initialize())
    {
        MessageBoxW(hwnd, L"Failed to initialize audio (XAudio2). Preview playback will be unavailable.",
                    L"RhythmEditor", MB_OK | MB_ICONWARNING);
    }

    m_leftColumnWidth = m_settings.LoadPaneSize(L"LeftColumnWidth", -1.0f);
    m_songPaneHeight = m_settings.LoadPaneSize(L"SongPaneHeight", 180.0f);
    m_timelineHeight = m_settings.LoadPaneSize(L"TimelineHeight", 220.0f);
    m_bottomHeight = m_settings.LoadPaneSize(L"BottomHeight", 160.0f);

    std::wstring lastPath = m_settings.LoadLastChartPath();
    if (!lastPath.empty())
    {
        std::vector<std::wstring> errors;
        if (EditorChartIO::LoadIntoDocument(lastPath, m_doc, errors))
        {
            m_hasDocument = true;
            m_undoHistory.Reset(m_doc);
            std::wstring prepError;
            m_blockPlayer.PrepareStems(m_doc, prepError);
            RebuildBlockSchedule();
        }
    }

    return true;
}

void EditorApp::Shutdown()
{
    m_audioEngine.Shutdown();
    if (m_comInitialized)
    {
        CoUninitialize();
    }
}

void EditorApp::RequestQuit()
{
    RequestActionWithGuard(PendingAction::Exit);
}

void EditorApp::Update()
{
    // WantTextInput guard applies to every shortcut below, New/Open/Exit
    // included: while a text field has keyboard focus, a bare Ctrl+letter
    // should never be stolen out from under normal typing/selection (e.g.
    // Ctrl+A to select-all in a text box).
    ImGuiIO& earlyIo = ImGui::GetIO();
    if (!earlyIo.WantTextInput)
    {
        if (earlyIo.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
        {
            RequestActionWithGuard(PendingAction::New);
        }
        if (earlyIo.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            RequestActionWithGuard(PendingAction::Open);
        }
        if (m_hasDocument && earlyIo.KeyCtrl && !earlyIo.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            DoSave();
        }
        if (m_hasDocument && earlyIo.KeyCtrl && earlyIo.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            DoSaveAs();
        }
    }

    if (m_hasDocument)
    {
        m_blockPlayer.Update(ImGui::GetIO().DeltaTime);

        ImGuiIO& io = ImGui::GetIO();
        // WantTextInput guard: while a text field has keyboard focus, let
        // ImGui's own per-widget text-undo handle Ctrl+Z instead of jumping
        // to a full document-level undo out from under an in-progress edit.
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            DoUndo();
        }
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
        {
            DoRedo();
        }
        // Spacebar: the universal transport convention (DAWs, video
        // editors, chart tools) for toggling play/pause - WantTextInput
        // guard so typing a literal space into a text field never doubles
        // as a transport command.
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        {
            if (m_blockPlayer.IsPlaying())
            {
                m_blockPlayer.Pause();
            }
            else
            {
                m_blockPlayer.Play();
            }
        }

        if (m_doc.docVersion != m_observedVersion)
        {
            m_observedVersion = m_doc.docVersion;
            m_lastEditTimeMs = GetTickCount64();
        }
        // Debounced: only re-validate (and rebuild the block schedule,
        // which is what keeps every block's displayed timing - e.g. a
        // clip's hits_required changing - in sync automatically) ~300ms
        // after the last edit. Short enough to feel immediate once the
        // user pauses, long enough that typing never triggers file I/O
        // mid-keystroke.
        if (m_observedVersion != m_lastValidatedVersion && GetTickCount64() - m_lastEditTimeMs > 300)
        {
            EditorChartIO::ValidateDocument(m_doc, m_currentErrors);
            m_lastValidatedVersion = m_observedVersion;
            RebuildBlockSchedule();
        }
    }

    ApplyWindowTitleIfChanged();

    DrawMenuBar();

    ImGuiIO& io = ImGui::GetIO();
    float menuBarHeight = ImGui::GetFrameHeight();
    float contentY = menuBarHeight;
    float contentWidth = io.DisplaySize.x;
    float contentHeight = io.DisplaySize.y - menuBarHeight;

    constexpr float kSplitterThickness = 6.0f;
    constexpr float kMinPaneSize = 80.0f;

    if (m_leftColumnWidth <= 0.0f)
    {
        m_leftColumnWidth = contentWidth * 0.4f;
    }

    // Every pane size below is clamped into a *local* copy for this frame's
    // layout only - never written back into the m_* members (SaveLayoutSettings'
    // source of truth) except where a splitter is actually being dragged
    // right now (see draggedThisFrame below). io.DisplaySize can be too
    // small to fit the preferred sizes for a single transient frame without
    // the window itself really having shrunk to that size - not just an
    // intentional resize, but also the few frames Windows reports a small,
    // not-yet-settled client rect while a minimize/restore animation is
    // still playing (EditorMain.cpp's IsIconic guard only covers the
    // steady-state minimized case, not that animated transition, and
    // there's no reliable way to catch every such transient from this side
    // instead). Clamping only a local copy means a transient frame renders
    // a correctly-fitted-but-temporary layout without corrupting what's
    // remembered - the preferred sizes are exactly what's still in m_* the
    // moment io.DisplaySize is too, with no manual drag needed to recover
    // them, and SaveLayoutSettings never sees the transient value at all.
    float timelineHeight = m_timelineHeight;
    float bottomHeight = m_bottomHeight;
    float leftColumnWidth = m_leftColumnWidth;
    float songPaneHeight = m_songPaneHeight;

    if (timelineHeight < kMinPaneSize)
    {
        timelineHeight = kMinPaneSize;
    }
    if (bottomHeight < kMinPaneSize)
    {
        bottomHeight = kMinPaneSize;
    }
    float maxTimelinePlusBottom = contentHeight - kMinPaneSize - kSplitterThickness * 2.0f;
    if (timelineHeight + bottomHeight > maxTimelinePlusBottom)
    {
        float excess = (timelineHeight + bottomHeight) - maxTimelinePlusBottom;
        float takeFromTimeline = timelineHeight - kMinPaneSize;
        if (takeFromTimeline > excess)
        {
            takeFromTimeline = excess;
        }
        if (takeFromTimeline > 0.0f)
        {
            timelineHeight -= takeFromTimeline;
            excess -= takeFromTimeline;
        }
        if (excess > 0.0f)
        {
            bottomHeight -= excess;
            if (bottomHeight < kMinPaneSize)
            {
                bottomHeight = kMinPaneSize;
            }
        }
    }

    float upperHeight = contentHeight - timelineHeight - bottomHeight - kSplitterThickness * 2.0f;
    if (upperHeight < kMinPaneSize)
    {
        upperHeight = kMinPaneSize;
    }

    if (leftColumnWidth < kMinPaneSize)
    {
        leftColumnWidth = kMinPaneSize;
    }
    float maxLeftColumnWidth = contentWidth - kMinPaneSize - kSplitterThickness;
    if (leftColumnWidth > maxLeftColumnWidth)
    {
        leftColumnWidth = maxLeftColumnWidth > kMinPaneSize ? maxLeftColumnWidth : kMinPaneSize;
    }
    float rightColumnX = leftColumnWidth + kSplitterThickness;
    float rightColumnWidth = contentWidth - rightColumnX;

    if (songPaneHeight < kMinPaneSize)
    {
        songPaneHeight = kMinPaneSize;
    }
    float maxSongPaneHeight = upperHeight - kMinPaneSize - kSplitterThickness;
    if (songPaneHeight > maxSongPaneHeight)
    {
        songPaneHeight = maxSongPaneHeight > kMinPaneSize ? maxSongPaneHeight : kMinPaneSize;
    }
    float clipsY = contentY + songPaneHeight + kSplitterThickness;
    float clipsHeight = upperHeight - songPaneHeight - kSplitterThickness;

    float timelineY = contentY + upperHeight + kSplitterThickness;
    float bottomY = timelineY + timelineHeight + kSplitterThickness;

    DrawSongPropertiesWindow(0.0f, contentY, leftColumnWidth, songPaneHeight);
    DrawClipsWindow(0.0f, clipsY, leftColumnWidth, clipsHeight);
    DrawBlockPropertiesWindow(rightColumnX, contentY, rightColumnWidth, upperHeight);
    DrawBlockTimelineWindow(0.0f, timelineY, contentWidth, timelineHeight);
    DrawBottomWindow(0.0f, bottomY, contentWidth, bottomHeight);

    if (m_hasDocument)
    {
        SyncClipSelectionFromBlock();
    }

    bool draggedThisFrame = false;

    // Left/right column boundary: left column is the stored value, right
    // column absorbs the change as a remainder. Deltas are applied on top
    // of this frame's (possibly clamped) displayed position, since that's
    // where the user visually grabbed the splitter from.
    float leftRightDelta = DrawPaneSplitter("##SplitLeftRight", ImVec2(leftColumnWidth, contentY),
                                             ImVec2(kSplitterThickness, upperHeight), false);
    if (leftRightDelta != 0.0f)
    {
        m_leftColumnWidth = leftColumnWidth + leftRightDelta;
        draggedThisFrame = true;
    }

    // Song/Clips boundary within the left column: Song is the stored
    // value, Clips absorbs the change as a remainder.
    float songClipsDelta = DrawPaneSplitter("##SplitSongClips", ImVec2(0.0f, clipsY - kSplitterThickness),
                                             ImVec2(leftColumnWidth, kSplitterThickness), true);
    if (songClipsDelta != 0.0f)
    {
        m_songPaneHeight = songPaneHeight + songClipsDelta;
        draggedThisFrame = true;
    }

    // Upper row/Timeline boundary: upperHeight is itself a remainder
    // (contentHeight minus timeline and bottom), so dragging this one only
    // needs to move m_timelineHeight - upperHeight absorbs the change.
    float upperTimelineDelta = DrawPaneSplitter("##SplitUpperTimeline", ImVec2(0.0f, timelineY - kSplitterThickness),
                                                 ImVec2(contentWidth, kSplitterThickness), true);
    if (upperTimelineDelta != 0.0f)
    {
        m_timelineHeight = timelineHeight - upperTimelineDelta;
        draggedThisFrame = true;
    }

    // Timeline/Bottom boundary: both are independently stored, so this one
    // transfers pixels directly between them (upperHeight is untouched).
    float timelineBottomDelta = DrawPaneSplitter("##SplitTimelineBottom", ImVec2(0.0f, bottomY - kSplitterThickness),
                                                  ImVec2(contentWidth, kSplitterThickness), true);
    if (timelineBottomDelta != 0.0f)
    {
        m_timelineHeight = timelineHeight + timelineBottomDelta;
        m_bottomHeight = bottomHeight - timelineBottomDelta;
        draggedThisFrame = true;
    }

    if (draggedThisFrame)
    {
        m_layoutDirty = true;
    }
    if (m_layoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        SaveLayoutSettings();
        m_layoutDirty = false;
    }

    if (m_hasDocument)
    {
        m_undoHistory.RecordFrame(m_doc, ImGui::IsAnyItemActive());
    }

    DrawDirtyGuardModal();
    DrawErrorModal();
}

void EditorApp::DrawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New...", "Ctrl+N"))
            {
                RequestActionWithGuard(PendingAction::New);
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
            {
                RequestActionWithGuard(PendingAction::Open);
            }
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_hasDocument))
            {
                DoSave();
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_hasDocument))
            {
                DoSaveAs();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
            {
                RequestActionWithGuard(PendingAction::Exit);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_hasDocument && m_undoHistory.CanUndo()))
            {
                DoUndo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_hasDocument && m_undoHistory.CanRedo()))
            {
                DoRedo();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

namespace
{
constexpr ImGuiWindowFlags kFixedPanelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus;
} // namespace

void EditorApp::DrawSongPropertiesWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("SongPropertiesWindow", nullptr, kFixedPanelFlags);
    ImGui::Text("Song");
    ImGui::Separator();
    if (m_hasDocument)
    {
        m_songPropertiesPanel.Draw(m_doc, m_blockPlayer);
    }
    else
    {
        ImGui::TextDisabled("No document open - File > New or Open.");
    }
    ImGui::End();
}

void EditorApp::DrawClipsWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("ClipsWindow", nullptr, kFixedPanelFlags);
    ImGui::Text("Clips");
    ImGui::Separator();
    if (m_hasDocument)
    {
        m_clipPanel.Draw(m_doc, m_hwnd);
    }
    else
    {
        ImGui::TextDisabled("No document open - File > New or Open.");
    }
    ImGui::End();
}

void EditorApp::DrawBlockPropertiesWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("BlockPropertiesWindow", nullptr, kFixedPanelFlags);
    ImGui::Text("Block Properties");
    ImGui::Separator();
    if (m_hasDocument)
    {
        // BlockPropertiesPanel only ever shows/edits the primary selection
        // below - call that out explicitly when more blocks than that are
        // highlighted, so it's clear Delete/Copy on the timeline act on the
        // whole group even though the fields shown here are just the one.
        size_t selectedCount = m_blockTimeline.MultiSelectedBlockIds().size();
        if (selectedCount > 1)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%d blocks selected - Delete/Copy act on all of them",
                                static_cast<int>(selectedCount));
            ImGui::Separator();
        }
        int newSelection = m_blockPropertiesPanel.Draw(m_doc, m_blockTimeline.SelectedBlockId(), m_blockPlayer);
        m_blockTimeline.SetSelectedBlockId(newSelection);
    }
    else
    {
        ImGui::TextDisabled("No document open - File > New or Open.");
    }
    ImGui::End();
}

void EditorApp::DrawBlockTimelineWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("BlockTimelineWindow", nullptr, kFixedPanelFlags);
    ImGui::Text("Blocks");
    ImGui::Separator();
    if (m_hasDocument)
    {
        m_blockTimeline.Draw(m_doc, m_blockPlayer);
    }
    else
    {
        ImGui::TextDisabled("No document open - File > New or Open.");
    }
    ImGui::End();
}

void EditorApp::DrawBottomWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("BottomWindow", nullptr, kFixedPanelFlags);

    ImGui::BeginChild("Transport", ImVec2(w * 0.4f, 0), true);
    ImGui::Text("Preview");
    ImGui::Separator();
    if (m_hasDocument)
    {
        if (ImGui::Button(m_blockPlayer.IsPlaying() ? "Pause" : "Play"))
        {
            if (m_blockPlayer.IsPlaying())
            {
                m_blockPlayer.Pause();
            }
            else
            {
                m_blockPlayer.Play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            m_blockPlayer.Stop();
        }
        bool loop = m_blockPlayer.LoopWholeSong();
        if (ImGui::Checkbox("Loop whole song", &loop))
        {
            m_blockPlayer.SetLoopWholeSong(loop);
        }

        const BlockSchedule::Schedule* schedule = m_blockPlayer.CurrentSchedule();
        if (schedule != nullptr && !schedule->entries.empty())
        {
            // Assumes perfect learning - the same assumption the whole
            // block schedule/playhead is built on (see BlockSchedule.h).
            ImGui::Text("Estimated length (perfect play): %s", ToUtf8(FormatMinutesSeconds(schedule->totalSeconds)).c_str());
        }
        else
        {
            ImGui::TextDisabled("Estimated length: (unavailable - fix validation problems first)");
        }

        ImGui::TextWrapped("%s", ToUtf8(m_blockPlayer.NowPlayingText()).c_str());
    }
    else
    {
        ImGui::TextDisabled("No document open.");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("Problems", ImVec2(0, 0), true);
    if (m_currentErrors.empty())
    {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Valid");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%d problem(s)", static_cast<int>(m_currentErrors.size()));
    }
    ImGui::SameLine();
    bool canValidate = m_hasDocument;
    if (!canValidate)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Validate Now"))
    {
        EditorChartIO::ValidateDocument(m_doc, m_currentErrors);
        m_lastValidatedVersion = m_doc.docVersion;
        RebuildBlockSchedule();
    }
    if (!canValidate)
    {
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    for (const std::wstring& err : m_currentErrors)
    {
        ImGui::TextWrapped("%s", ToUtf8(err).c_str());
    }
    ImGui::EndChild();

    ImGui::End();
}

void EditorApp::DrawDirtyGuardModal()
{
    if (m_showDirtyGuardModal)
    {
        ImGui::OpenPopup("Unsaved changes");
        m_showDirtyGuardModal = false;
    }

    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Save changes to '%s' before continuing?", ToUtf8(m_doc.title).c_str());
        if (ImGui::Button("Save"))
        {
            std::vector<std::wstring> errors;
            if (EditorChartIO::SaveDocument(m_doc, errors))
            {
                m_settings.SaveLastChartPath(m_doc.chartFilePath);
                RunPendingAction();
            }
            else
            {
                m_pendingAction = PendingAction::None;
                ShowErrorModal("Could not save chart", errors);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            RunPendingAction();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_pendingAction = PendingAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::ShowErrorModal(const std::string& title, const std::vector<std::wstring>& messages)
{
    m_errorModalTitle = title;
    m_errorModalMessages = messages;
    m_showErrorModalFlag = true;
}

void EditorApp::DrawErrorModal()
{
    if (m_showErrorModalFlag)
    {
        ImGui::OpenPopup("##errorModal");
        m_showErrorModalFlag = false;
    }

    if (ImGui::BeginPopupModal("##errorModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", m_errorModalTitle.c_str());
        ImGui::Separator();
        for (const std::wstring& msg : m_errorModalMessages)
        {
            ImGui::TextWrapped("%s", ToUtf8(msg).c_str());
        }
        if (ImGui::Button("OK"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::ApplyWindowTitleIfChanged()
{
    std::wstring title = L"RhythmEditor";
    if (m_hasDocument)
    {
        title = (m_doc.title.empty() ? L"(untitled)" : m_doc.title) + (m_doc.dirty ? L"*" : L"") + L" - RhythmEditor";
    }
    if (title != m_lastAppliedTitle)
    {
        SetWindowTextW(m_hwnd, title.c_str());
        m_lastAppliedTitle = title;
    }
}

void EditorApp::SaveLayoutSettings()
{
    m_settings.SavePaneSize(L"LeftColumnWidth", m_leftColumnWidth);
    m_settings.SavePaneSize(L"SongPaneHeight", m_songPaneHeight);
    m_settings.SavePaneSize(L"TimelineHeight", m_timelineHeight);
    m_settings.SavePaneSize(L"BottomHeight", m_bottomHeight);
}

void EditorApp::RebuildBlockSchedule()
{
    if (!m_hasDocument)
    {
        return;
    }
    // BlockPlayer::RebuildSchedule leaves the previous schedule in place on
    // failure (see its own doc comment) - errors here are already
    // reflected in m_currentErrors via the validation pass that triggered
    // this call, so there's nothing extra to surface.
    std::vector<std::wstring> scheduleErrors;
    m_blockPlayer.RebuildSchedule(m_doc, scheduleErrors);
}

void EditorApp::SyncClipSelectionFromBlock()
{
    int selectedBlockId = m_blockTimeline.SelectedBlockId();
    if (selectedBlockId == m_lastSyncedBlockSelectionId)
    {
        return;
    }
    m_lastSyncedBlockSelectionId = selectedBlockId;

    for (const EditorBlock& block : m_doc.blocks)
    {
        if (block.id == selectedBlockId)
        {
            m_clipPanel.SetSelectedClipId(block.clipId);
            break;
        }
    }
}

void EditorApp::DoNew()
{
    std::wstring folder;
    if (!FileDialogs::PickFolder(m_hwnd, folder))
    {
        return;
    }
    folder = StripTrailingSlash(folder) + L"\\";
    std::wstring name = LastPathComponent(folder);

    m_blockPlayer.Stop();

    EditorDocument doc;
    doc.chartFilePath = folder + name + L".chart";
    doc.folderPath = folder;
    doc.title = name;
    doc.dirty = true;

    m_doc = doc;
    m_hasDocument = true;
    m_undoHistory.Reset(m_doc);
    m_songPropertiesPanel.NotifyDocumentReplaced();
    m_observedVersion = m_doc.docVersion;
    m_lastValidatedVersion = m_doc.docVersion - 1; // force a prompt validation pass
    m_currentErrors.clear();

    std::wstring prepError;
    m_blockPlayer.PrepareStems(m_doc, prepError);
    RebuildBlockSchedule();
}

void EditorApp::DoOpen()
{
    std::wstring path;
    if (!FileDialogs::PickOpenFile(m_hwnd, L"Chart files", L"*.chart", path))
    {
        return;
    }

    EditorDocument newDoc;
    std::vector<std::wstring> errors;
    if (!EditorChartIO::LoadIntoDocument(path, newDoc, errors))
    {
        ShowErrorModal("Could not open chart - it doesn't currently pass validation:", errors);
        return;
    }

    m_blockPlayer.Stop();
    m_doc = newDoc;
    m_hasDocument = true;
    m_undoHistory.Reset(m_doc);
    m_songPropertiesPanel.NotifyDocumentReplaced();
    m_observedVersion = m_doc.docVersion;
    m_lastValidatedVersion = m_doc.docVersion - 1;
    m_currentErrors.clear();
    m_settings.SaveLastChartPath(path);

    std::wstring prepError;
    m_blockPlayer.PrepareStems(m_doc, prepError);
    RebuildBlockSchedule();
}

void EditorApp::DoSave()
{
    if (!m_hasDocument)
    {
        return;
    }
    std::vector<std::wstring> errors;
    if (!EditorChartIO::SaveDocument(m_doc, errors))
    {
        ShowErrorModal("Could not save chart:", errors);
        return;
    }
    m_settings.SaveLastChartPath(m_doc.chartFilePath);
    m_currentErrors.clear();
    m_lastValidatedVersion = m_doc.docVersion;
}

void EditorApp::DoSaveAs()
{
    if (!m_hasDocument)
    {
        return;
    }
    std::wstring path;
    std::wstring defaultName = (m_doc.title.empty() ? std::wstring(L"chart") : m_doc.title) + L".chart";
    if (!FileDialogs::PickSaveFile(m_hwnd, L"Chart files", L"*.chart", defaultName.c_str(), path))
    {
        return;
    }

    m_blockPlayer.Stop();
    std::vector<std::wstring> errors;
    if (!EditorChartIO::SaveDocumentAs(m_doc, path, errors))
    {
        ShowErrorModal("Could not save chart:", errors);
        return;
    }
    m_settings.SaveLastChartPath(m_doc.chartFilePath);
    m_currentErrors.clear();
    m_lastValidatedVersion = m_doc.docVersion;

    std::wstring prepError;
    m_blockPlayer.PrepareStems(m_doc, prepError);
    RebuildBlockSchedule();
}

void EditorApp::DoUndo()
{
    if (!m_undoHistory.CanUndo())
    {
        return;
    }
    m_undoHistory.Undo(m_doc);
    m_clipPanel.NotifyDocumentReplaced();
    m_songPropertiesPanel.NotifyDocumentReplaced();
}

void EditorApp::DoRedo()
{
    if (!m_undoHistory.CanRedo())
    {
        return;
    }
    m_undoHistory.Redo(m_doc);
    m_clipPanel.NotifyDocumentReplaced();
    m_songPropertiesPanel.NotifyDocumentReplaced();
}

void EditorApp::RequestActionWithGuard(PendingAction action)
{
    m_pendingAction = action;
    if (m_hasDocument && m_doc.dirty)
    {
        m_showDirtyGuardModal = true;
    }
    else
    {
        RunPendingAction();
    }
}

void EditorApp::RunPendingAction()
{
    PendingAction action = m_pendingAction;
    m_pendingAction = PendingAction::None;
    switch (action)
    {
        case PendingAction::New:
            DoNew();
            break;
        case PendingAction::Open:
            DoOpen();
            break;
        case PendingAction::Exit:
            m_wantsToQuit = true;
            break;
        case PendingAction::None:
            break;
    }
}
