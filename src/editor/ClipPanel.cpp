#include "ClipPanel.h"

#include <imgui.h>

#include <cstring>

#include "ChartMidi.h"
#include "FileDialogs.h"
#include "Utf8.h"

namespace
{

std::wstring GenerateUniqueClipName(const EditorDocument& doc, const std::wstring& base)
{
    auto nameTaken = [&](const std::wstring& candidate)
    {
        for (const EditorClip& clip : doc.clips)
        {
            if (clip.name == candidate)
            {
                return true;
            }
        }
        return false;
    };

    if (!nameTaken(base))
    {
        return base;
    }
    for (int i = 2;; ++i)
    {
        std::wstring candidate = base + L"_" + std::to_wstring(i);
        if (!nameTaken(candidate))
        {
            return candidate;
        }
    }
}

bool FileExistsOnDisk(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Draws an override-toggle + value pair for one of a clip's two optional
// tolerance fields: unchecked shows the song's current default, grayed out.
void DrawToleranceField(const char* label, Overridable<double>& field, double songDefault, EditorDocument& doc)
{
    ImGui::PushID(label);
    bool overridden = field.isOverridden;
    if (ImGui::Checkbox("##override", &overridden))
    {
        field.isOverridden = overridden;
        if (overridden && field.value <= 0.0)
        {
            field.value = songDefault;
        }
        MarkDirty(doc);
    }
    ImGui::SameLine();
    float value = static_cast<float>(field.isOverridden ? field.value : songDefault);
    if (!field.isOverridden)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputFloat(label, &value, 0.0f, 0.0f, "%.1f") && field.isOverridden)
    {
        if (value <= 0.0f)
        {
            value = 0.1f;
        }
        field.value = value;
        MarkDirty(doc);
    }
    if (!field.isOverridden)
    {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

} // namespace

void ClipPanel::Draw(EditorDocument& doc, HWND owner)
{
    ImGui::BeginChild("ClipList", ImVec2(220, 0), true);
    DrawList(doc);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ClipInspector", ImVec2(0, 0), true);
    DrawInspector(doc, owner);
    ImGui::EndChild();

    DrawRenameModal(doc);
    DrawOverwriteModal(doc);
    DrawDeleteModal(doc);
}

void ClipPanel::DrawList(EditorDocument& doc)
{
    if (ImGui::Button("New"))
    {
        CreateNewClip(doc);
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate"))
    {
        DuplicateSelected(doc);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && m_selectedClipId >= 0)
    {
        RequestDelete(doc, m_selectedClipId);
    }
    ImGui::Separator();

    for (const EditorClip& clip : doc.clips)
    {
        bool referenced = false;
        for (const EditorSection& section : doc.sections)
        {
            if (section.clipId == clip.id)
            {
                referenced = true;
                break;
            }
        }

        std::string label = ToUtf8(clip.displayName.empty() ? std::wstring(L"(unnamed)") : clip.displayName);
        if (!clip.hasWav)
        {
            label += " [no wav]";
        }
        if (!referenced)
        {
            label += " [unused]";
        }

        ImGui::PushID(clip.id);
        if (ImGui::Selectable(label.c_str(), m_selectedClipId == clip.id))
        {
            m_selectedClipId = clip.id;
        }
        ImGui::PopID();
    }
}

void ClipPanel::DrawInspector(EditorDocument& doc, HWND owner)
{
    EditorClip* clip = FindClipById(doc, m_selectedClipId);
    if (clip == nullptr)
    {
        ImGui::TextDisabled("Select a clip on the left, or create a new one.");
        return;
    }

    SyncScratchIfNeeded(*clip);

    ImGui::Text("Display Name");
    if (ImGui::InputText("##displayName", m_displayNameBuf, sizeof(m_displayNameBuf)))
    {
        clip->displayName = FromUtf8(m_displayNameBuf);
        MarkDirty(doc);
    }
    if (clip->displayName.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "display_name is required");
    }

    ImGui::Text("Name (also the file name)");
    if (ImGui::InputText("##name", m_nameBuf, sizeof(m_nameBuf)))
    {
        // Live-typed value only; the actual commit (and any rename-on-disk
        // prompt) happens once editing finishes, below.
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        CommitNameChange(doc, *clip, FromUtf8(m_nameBuf));
    }
    std::wstring liveTypedName = FromUtf8(m_nameBuf);
    if (liveTypedName.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "name is required");
    }
    else
    {
        for (const EditorClip& other : doc.clips)
        {
            if (other.id != clip->id && other.name == liveTypedName)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "another clip already uses this name");
                break;
            }
        }
    }

    ImGui::Separator();

    int hitsRequired = clip->hitsRequired;
    if (ImGui::InputInt("Hits Required", &hitsRequired))
    {
        if (hitsRequired < 1)
        {
            hitsRequired = 1;
        }
        clip->hitsRequired = hitsRequired;
        MarkDirty(doc);
    }

    float initVolume = static_cast<float>(clip->initVolume);
    if (ImGui::SliderFloat("Init Volume", &initVolume, 0.0f, 2.0f))
    {
        clip->initVolume = initVolume < 0.0f ? 0.0 : initVolume;
        MarkDirty(doc);
    }

    float volume = static_cast<float>(clip->volume);
    if (ImGui::SliderFloat("Volume", &volume, 0.0f, 2.0f))
    {
        clip->volume = volume < 0.0f ? 0.0 : volume;
        MarkDirty(doc);
    }

    ImGui::Separator();
    ImGui::Text("Timing Tolerances");
    DrawToleranceField("Start Tolerance (ms)", clip->startToleranceMs, doc.startToleranceMs, doc);
    DrawToleranceField("Release Tolerance (ms)", clip->releaseToleranceMs, doc.releaseToleranceMs, doc);

    ImGui::Separator();
    ImGui::Text("Audio / MIDI");

    bool canImport = !liveTypedName.empty();
    if (!canImport)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(clip->hasWav ? "Re-import WAV..." : "Import WAV..."))
    {
        ImportFile(doc, *clip, false, owner);
    }
    ImGui::SameLine();
    if (ImGui::Button(clip->hasMidi ? "Re-import MIDI..." : "Import MIDI..."))
    {
        ImportFile(doc, *clip, true, owner);
    }
    if (!canImport)
    {
        ImGui::EndDisabled();
        ImGui::TextDisabled("Set a name before importing audio/MIDI.");
    }

    ImGui::Text("WAV: %s", clip->hasWav ? "imported" : "none");
    if (clip->hasMidi)
    {
        int totalNotes = 0;
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            totalNotes += static_cast<int>(clip->laneNotes[lane].size());
        }
        ImGui::Text("MIDI: %d note(s), spans %.2f beats", totalNotes, clip->spanBeats);
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            ImGui::BulletText("Lane %d (pitch %d): %d note(s)", lane, kLaneMidiPitches[lane],
                               static_cast<int>(clip->laneNotes[lane].size()));
        }
    }
    else
    {
        ImGui::Text("MIDI: none (only usable in Break/Background/Reset)");
    }
    if (!m_lastMidiImportError.empty() && clip->id == m_selectedClipId)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Last MIDI import warning: %s", ToUtf8(m_lastMidiImportError).c_str());
    }
}

void ClipPanel::SyncScratchIfNeeded(const EditorClip& clip)
{
    if (m_scratchForClipId == clip.id)
    {
        return;
    }
    m_scratchForClipId = clip.id;

    std::string name = ToUtf8(clip.name);
    std::string displayName = ToUtf8(clip.displayName);
    // Truncate to the buffer's capacity rather than overflow - clip names
    // this long would be unreasonable in practice.
    size_t nameLen = name.size() < sizeof(m_nameBuf) - 1 ? name.size() : sizeof(m_nameBuf) - 1;
    size_t displayLen = displayName.size() < sizeof(m_displayNameBuf) - 1 ? displayName.size() : sizeof(m_displayNameBuf) - 1;
    memcpy(m_nameBuf, name.data(), nameLen);
    m_nameBuf[nameLen] = '\0';
    memcpy(m_displayNameBuf, displayName.data(), displayLen);
    m_displayNameBuf[displayLen] = '\0';
    m_lastMidiImportError.clear();
}

void ClipPanel::CommitNameChange(EditorDocument& doc, EditorClip& clip, const std::wstring& newName)
{
    if (newName == clip.name)
    {
        return;
    }
    if (newName.empty())
    {
        // Revert the buffer to the last committed name - an empty name is
        // never valid, so there's nothing to prompt about.
        m_scratchForClipId = -1;
        return;
    }

    if (clip.hasWav || clip.hasMidi)
    {
        m_showRenameModal = true;
        m_renameClipId = clip.id;
        m_renameOldName = clip.name;
        m_renameNewName = newName;
    }
    else
    {
        clip.name = newName;
        MarkDirty(doc);
        m_scratchForClipId = -1;
    }
}

void ClipPanel::DrawRenameModal(EditorDocument& doc)
{
    if (m_showRenameModal)
    {
        ImGui::OpenPopup("Rename clip files?");
        m_showRenameModal = false;
    }

    if (ImGui::BeginPopupModal("Rename clip files?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        EditorClip* clip = FindClipById(doc, m_renameClipId);
        if (clip == nullptr)
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::Text("Renaming '%s' to '%s'.", ToUtf8(m_renameOldName).c_str(), ToUtf8(m_renameNewName).c_str());
        ImGui::TextWrapped("This clip's files are named after its 'name' field. Rename %s.wav/.mid on disk too?",
                            ToUtf8(m_renameOldName).c_str());

        if (ImGui::Button("Rename files"))
        {
            bool ok = true;
            if (clip->hasWav)
            {
                std::wstring oldPath = doc.folderPath + m_renameOldName + L".wav";
                std::wstring newPath = doc.folderPath + m_renameNewName + L".wav";
                ok = ok && MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            if (ok && clip->hasMidi)
            {
                std::wstring oldPath = doc.folderPath + m_renameOldName + L".mid";
                std::wstring newPath = doc.folderPath + m_renameNewName + L".mid";
                ok = ok && MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            if (ok)
            {
                clip->name = m_renameNewName;
                MarkDirty(doc);
            }
            m_scratchForClipId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Just relabel"))
        {
            clip->name = m_renameNewName;
            clip->hasWav = false;
            clip->hasMidi = false;
            MarkDirty(doc);
            m_scratchForClipId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_scratchForClipId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ClipPanel::ImportFile(EditorDocument& doc, EditorClip& clip, bool isMidi, HWND owner)
{
    std::wstring srcPath;
    bool picked = isMidi ? FileDialogs::PickOpenFile(owner, L"MIDI files", L"*.mid;*.midi", srcPath)
                          : FileDialogs::PickOpenFile(owner, L"WAV files", L"*.wav", srcPath);
    if (!picked)
    {
        return;
    }

    std::wstring dstPath = doc.folderPath + clip.name + (isMidi ? L".mid" : L".wav");
    if (FileExistsOnDisk(dstPath) && dstPath != srcPath)
    {
        m_showOverwriteModal = true;
        m_overwriteClipId = clip.id;
        m_overwriteSrc = srcPath;
        m_overwriteDst = dstPath;
        m_overwriteIsMidi = isMidi;
        return;
    }

    CompleteImport(doc, clip, srcPath, dstPath, isMidi);
}

void ClipPanel::CompleteImport(EditorDocument& doc, EditorClip& clip, const std::wstring& src, const std::wstring& dst, bool isMidi)
{
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
    {
        return;
    }

    if (isMidi)
    {
        clip.hasMidi = true;
        m_lastMidiImportError.clear();
        MidiLaneData midiData;
        std::wstring midiError;
        if (ChartMidi::LoadLaneNotes(dst, midiData, midiError))
        {
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                clip.laneNotes[lane] = midiData.lanes[lane];
            }
            clip.spanBeats = midiData.totalBeats;
        }
        else
        {
            m_lastMidiImportError = midiError;
        }
    }
    else
    {
        clip.hasWav = true;
    }
    MarkDirty(doc);
}

void ClipPanel::DrawOverwriteModal(EditorDocument& doc)
{
    if (m_showOverwriteModal)
    {
        ImGui::OpenPopup("Overwrite existing file?");
        m_showOverwriteModal = false;
    }

    if (ImGui::BeginPopupModal("Overwrite existing file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("This will overwrite %s.", ToUtf8(m_overwriteDst).c_str());
        if (ImGui::Button("Overwrite"))
        {
            EditorClip* clip = FindClipById(doc, m_overwriteClipId);
            if (clip != nullptr)
            {
                CompleteImport(doc, *clip, m_overwriteSrc, m_overwriteDst, m_overwriteIsMidi);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ClipPanel::CreateNewClip(EditorDocument& doc)
{
    EditorClip clip;
    clip.id = doc.nextClipId++;
    clip.name = GenerateUniqueClipName(doc, L"clip");
    clip.displayName = L"New Clip";
    doc.clips.push_back(clip);
    m_selectedClipId = clip.id;
    MarkDirty(doc);
}

void ClipPanel::DuplicateSelected(EditorDocument& doc)
{
    const EditorClip* src = FindClipById(doc, m_selectedClipId);
    if (src == nullptr)
    {
        return;
    }

    EditorClip copy = *src;
    copy.id = doc.nextClipId++;
    copy.name = GenerateUniqueClipName(doc, src->name + L"_copy");
    copy.displayName = src->displayName + L" Copy";
    copy.hasWav = false;
    copy.hasMidi = false;

    if (src->hasWav)
    {
        std::wstring oldWav = doc.folderPath + src->name + L".wav";
        std::wstring newWav = doc.folderPath + copy.name + L".wav";
        copy.hasWav = CopyFileW(oldWav.c_str(), newWav.c_str(), FALSE) != 0;
    }
    if (src->hasMidi)
    {
        std::wstring oldMid = doc.folderPath + src->name + L".mid";
        std::wstring newMid = doc.folderPath + copy.name + L".mid";
        copy.hasMidi = CopyFileW(oldMid.c_str(), newMid.c_str(), FALSE) != 0;
    }

    doc.clips.push_back(copy);
    m_selectedClipId = copy.id;
    MarkDirty(doc);
}

void ClipPanel::RequestDelete(EditorDocument& doc, int clipId)
{
    bool referenced = false;
    for (const EditorSection& section : doc.sections)
    {
        if (section.clipId == clipId)
        {
            referenced = true;
            break;
        }
    }

    if (!referenced)
    {
        for (size_t i = 0; i < doc.clips.size(); ++i)
        {
            if (doc.clips[i].id == clipId)
            {
                doc.clips.erase(doc.clips.begin() + static_cast<long>(i));
                break;
            }
        }
        if (m_selectedClipId == clipId)
        {
            m_selectedClipId = -1;
        }
        MarkDirty(doc);
        return;
    }

    m_showDeleteModal = true;
    m_deleteClipId = clipId;
}

void ClipPanel::DrawDeleteModal(EditorDocument& doc)
{
    if (m_showDeleteModal)
    {
        ImGui::OpenPopup("Clip is in use");
        m_showDeleteModal = false;
    }

    if (ImGui::BeginPopupModal("Clip is in use", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This clip is used by:");
        int usedCount = 0;
        for (size_t i = 0; i < doc.sections.size(); ++i)
        {
            if (doc.sections[i].clipId == m_deleteClipId)
            {
                const wchar_t* kindName = L"Learn";
                switch (doc.sections[i].kind)
                {
                    case SectionKind::Learn:
                        kindName = L"Learn";
                        break;
                    case SectionKind::Break:
                        kindName = L"Break";
                        break;
                    case SectionKind::Reset:
                        kindName = L"Reset";
                        break;
                    case SectionKind::Background:
                        kindName = L"Background";
                        break;
                }
                ImGui::BulletText("%s #%d", ToUtf8(kindName).c_str(), static_cast<int>(i) + 1);
                ++usedCount;
            }
        }

        std::string deleteLabel = "Delete clip and remove " + std::to_string(usedCount) + " section(s)";
        if (ImGui::Button(deleteLabel.c_str()))
        {
            for (size_t i = doc.sections.size(); i-- > 0;)
            {
                if (doc.sections[i].clipId == m_deleteClipId)
                {
                    doc.sections.erase(doc.sections.begin() + static_cast<long>(i));
                }
            }
            for (size_t i = 0; i < doc.clips.size(); ++i)
            {
                if (doc.clips[i].id == m_deleteClipId)
                {
                    doc.clips.erase(doc.clips.begin() + static_cast<long>(i));
                    break;
                }
            }
            if (m_selectedClipId == m_deleteClipId)
            {
                m_selectedClipId = -1;
            }
            MarkDirty(doc);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
