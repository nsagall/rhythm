#include "EditorApp.h"

#include <objbase.h>

#include <imgui.h>

#include "EditorChartIO.h"
#include "FileDialogs.h"
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

    std::wstring lastPath = m_settings.LoadLastChartPath();
    if (!lastPath.empty())
    {
        std::vector<std::wstring> errors;
        if (EditorChartIO::LoadIntoDocument(lastPath, m_doc, errors))
        {
            m_hasDocument = true;
            std::wstring prepError;
            m_previewPlayer.PrepareForDocument(m_doc, prepError);
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
    if (m_hasDocument)
    {
        m_previewPlayer.Update();

        if (m_doc.docVersion != m_observedVersion)
        {
            m_observedVersion = m_doc.docVersion;
            m_lastEditTimeMs = GetTickCount64();
        }
        // Debounced: only re-validate ~750ms after the last edit, so
        // typing never triggers file I/O mid-keystroke.
        if (m_observedVersion != m_lastValidatedVersion && GetTickCount64() - m_lastEditTimeMs > 750)
        {
            EditorChartIO::ValidateDocument(m_doc, m_currentErrors);
            m_lastValidatedVersion = m_observedVersion;
        }
    }

    ApplyWindowTitleIfChanged();

    DrawMenuBar();

    ImGuiIO& io = ImGui::GetIO();
    float menuBarHeight = ImGui::GetFrameHeight();
    float bottomHeight = 200.0f;
    float contentY = menuBarHeight;
    float contentHeight = io.DisplaySize.y - menuBarHeight - bottomHeight;
    if (contentHeight < 100.0f)
    {
        contentHeight = 100.0f;
    }
    float leftWidth = io.DisplaySize.x * 0.45f;

    DrawClipsWindow(0.0f, contentY, leftWidth, contentHeight);
    DrawSectionsWindow(leftWidth, contentY, io.DisplaySize.x - leftWidth, contentHeight);
    DrawBottomWindow(0.0f, contentY + contentHeight, io.DisplaySize.x, bottomHeight);

    DrawDirtyGuardModal();
    DrawErrorModal();
}

void EditorApp::DrawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New..."))
            {
                RequestActionWithGuard(PendingAction::New);
            }
            if (ImGui::MenuItem("Open..."))
            {
                RequestActionWithGuard(PendingAction::Open);
            }
            if (ImGui::MenuItem("Save", nullptr, false, m_hasDocument))
            {
                DoSave();
            }
            if (ImGui::MenuItem("Save As...", nullptr, false, m_hasDocument))
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
        ImGui::EndMainMenuBar();
    }
}

namespace
{
constexpr ImGuiWindowFlags kFixedPanelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus;
} // namespace

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

void EditorApp::DrawSectionsWindow(float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("SectionsWindow", nullptr, kFixedPanelFlags);
    ImGui::Text("Section Sequence");
    ImGui::Separator();
    if (m_hasDocument)
    {
        m_sectionPanel.Draw(m_doc, &m_previewPlayer);
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
        if (ImGui::Button(m_previewPlayer.IsPlaying() ? "Pause" : "Play"))
        {
            if (m_previewPlayer.IsPlaying())
            {
                m_previewPlayer.Pause();
            }
            else
            {
                m_previewPlayer.Play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            m_previewPlayer.Stop();
        }
        bool loop = m_previewPlayer.LoopWholeSong();
        if (ImGui::Checkbox("Loop whole song", &loop))
        {
            m_previewPlayer.SetLoopWholeSong(loop);
        }
        ImGui::TextWrapped("%s", ToUtf8(m_previewPlayer.NowPlayingText()).c_str());
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

void EditorApp::DoNew()
{
    std::wstring folder;
    if (!FileDialogs::PickFolder(m_hwnd, folder))
    {
        return;
    }
    folder = StripTrailingSlash(folder) + L"\\";
    std::wstring name = LastPathComponent(folder);

    m_previewPlayer.Stop();

    EditorDocument doc;
    doc.chartFilePath = folder + name + L".chart";
    doc.folderPath = folder;
    doc.title = name;
    doc.dirty = true;

    m_doc = doc;
    m_hasDocument = true;
    m_observedVersion = m_doc.docVersion;
    m_lastValidatedVersion = m_doc.docVersion - 1; // force a prompt validation pass
    m_currentErrors.clear();

    std::wstring prepError;
    m_previewPlayer.PrepareForDocument(m_doc, prepError);
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

    m_previewPlayer.Stop();
    m_doc = newDoc;
    m_hasDocument = true;
    m_observedVersion = m_doc.docVersion;
    m_lastValidatedVersion = m_doc.docVersion - 1;
    m_currentErrors.clear();
    m_settings.SaveLastChartPath(path);

    std::wstring prepError;
    m_previewPlayer.PrepareForDocument(m_doc, prepError);
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

    m_previewPlayer.Stop();
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
    m_previewPlayer.PrepareForDocument(m_doc, prepError);
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
