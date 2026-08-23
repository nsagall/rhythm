#include "ClipPanel.h"

#include <imgui.h>

#include <cstring>
#include <cwctype>
#include <set>

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

// Turns a filename stem into a plausible display name - "drum_intro" ->
// "Drum Intro" - matching the convention already used throughout
// hand-authored Content/ charts. Only ever uppercases a word's own first
// letter (never forces the rest of a word to lowercase), so an
// intentional acronym like "hH" survives as "HH" rather than getting
// mangled into "Hh".
std::wstring PrettifyDisplayName(const std::wstring& base)
{
    std::wstring result = base;
    for (wchar_t& c : result)
    {
        if (c == L'_' || c == L'-')
        {
            c = L' ';
        }
    }
    bool capitalizeNext = true;
    for (wchar_t& c : result)
    {
        if (c == L' ')
        {
            capitalizeNext = true;
        }
        else if (capitalizeNext)
        {
            c = static_cast<wchar_t>(std::towupper(c));
            capitalizeNext = false;
        }
    }
    return result;
}

// Enumerates every *.wav / *.mid file directly inside folderPath, keyed by
// filename stem (extension stripped, case preserved as found on disk).
void ScanStemFiles(const std::wstring& folderPath, const wchar_t* pattern, std::set<std::wstring>& outStems)
{
    WIN32_FIND_DATAW findData;
    HANDLE handle = FindFirstFileW((folderPath + pattern).c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }
        std::wstring name = findData.cFileName;
        size_t dot = name.find_last_of(L'.');
        outStems.insert(dot == std::wstring::npos ? name : name.substr(0, dot));
    } while (FindNextFileW(handle, &findData));
    FindClose(handle);
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
    ImGui::BeginChild("ClipList", ImVec2(300, 0), true);
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
    ImGui::SameLine();
    if (ImGui::Button("Detect"))
    {
        DetectClips(doc);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Scan this song's folder for .wav/.mid files not yet used by any clip,\nand create a clip for each one found.");
    }
    if (!m_lastDetectSummary.empty())
    {
        ImGui::TextWrapped("%s", ToUtf8(m_lastDetectSummary).c_str());
    }
    ImGui::Separator();

    for (const EditorClip& clip : doc.clips)
    {
        bool referenced = false;
        for (const EditorBlock& block : doc.blocks)
        {
            if (block.clipId == clip.id)
            {
                referenced = true;
                break;
            }
        }

        std::string label = ToUtf8(clip.name.empty() ? std::wstring(L"(unnamed)") : clip.name);
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
        // Drag onto the block timeline to create a new block using this
        // clip (BlockTimeline::HandleClipDrop decides Learn vs. Background
        // based on clip.hasMidi).
        if (ImGui::BeginDragDropSource())
        {
            int clipId = clip.id;
            ImGui::SetDragDropPayload("CLIP_DRAG", &clipId, sizeof(int));
            ImGui::Text("%s", label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }

    // Delete key removes the selected clip without needing to reach for the
    // Delete button above - only while this list itself has focus (i.e. the
    // user just clicked a clip here), and never while a text field
    // elsewhere wants the keystroke. Goes through RequestDelete, so a clip
    // still in use by a block gets the same confirmation modal the button does.
    if (m_selectedClipId >= 0 && ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        RequestDelete(doc, m_selectedClipId);
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

    static const char* c_LearnModeNames[] = {"Pass", "Don't Fail"};
    int learnModeIndex = clip->learnMode == LearnMode::DontFail ? 1 : 0;
    if (ImGui::Combo("Learn Mode", &learnModeIndex, c_LearnModeNames, 2))
    {
        clip->learnMode = learnModeIndex == 1 ? LearnMode::DontFail : LearnMode::Pass;
        MarkDirty(doc);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Pass: a streak of Hits Required correct hits in a row locks in\n"
            "permanently - once reached, further misses never un-reach it.\n"
            "Don't Fail: the same streak is reversible - any miss drops it back\n"
            "to failing until Hits Required is re-earned. Only matters for a\n"
            "clip used in a [learn] block.");
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
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            totalNotes += static_cast<int>(clip->laneNotes[lane].size());
        }
        ImGui::Text("MIDI: %d note(s), spans %.2f beats", totalNotes, clip->spanBeats);
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            ImGui::BulletText("Lane %d (pitch %d): %d note(s)", lane, c_LaneMidiPitches[lane],
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
            for (int lane = 0; lane < c_LaneCount; ++lane)
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

void ClipPanel::DetectClips(EditorDocument& doc)
{
    if (doc.folderPath.empty())
    {
        m_lastDetectSummary = L"No song folder yet - use New or Open first.";
        return;
    }

    std::set<std::wstring> wavStems;
    std::set<std::wstring> midStems;
    ScanStemFiles(doc.folderPath, L"*.wav", wavStems);
    ScanStemFiles(doc.folderPath, L"*.mid", midStems);

    std::set<std::wstring> existingNames;
    for (const EditorClip& clip : doc.clips)
    {
        existingNames.insert(clip.name);
    }

    int created = 0;
    int lastCreatedClipId = -1;
    for (const std::wstring& stem : wavStems)
    {
        if (existingNames.count(stem) != 0)
        {
            continue; // already used by some clip - leave it alone
        }

        EditorClip clip;
        clip.id = doc.nextClipId++;
        clip.name = stem;
        clip.displayName = PrettifyDisplayName(stem);
        clip.hasWav = true;
        clip.hasMidi = midStems.count(stem) != 0;
        if (clip.hasMidi)
        {
            MidiLaneData midiData;
            std::wstring midiError;
            if (ChartMidi::LoadLaneNotes(doc.folderPath + stem + L".mid", midiData, midiError))
            {
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    clip.laneNotes[lane] = midiData.lanes[lane];
                }
                clip.spanBeats = midiData.totalBeats;
            }
            // A failure here just leaves the info-readout cache empty -
            // clip.hasMidi still reflects that a .mid file exists; whether
            // it's actually usable in a [learn] block is decided for real
            // at validate/save time, same as the Import MIDI button.
        }

        doc.clips.push_back(clip);
        existingNames.insert(stem);
        lastCreatedClipId = clip.id;
        ++created;
    }

    if (created > 0)
    {
        m_selectedClipId = lastCreatedClipId;
        MarkDirty(doc);
        m_lastDetectSummary =
            L"Found " + std::to_wstring(created) + (created == 1 ? L" new clip." : L" new clips.");
    }
    else
    {
        m_lastDetectSummary = L"No new .wav/.mid files found.";
    }
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
    for (const EditorBlock& block : doc.blocks)
    {
        if (block.clipId == clipId)
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
        for (size_t i = 0; i < doc.blocks.size(); ++i)
        {
            if (doc.blocks[i].clipId == m_deleteClipId)
            {
                const wchar_t* kindName = L"Learn";
                switch (doc.blocks[i].kind)
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

        std::string deleteLabel = "Delete clip and remove " + std::to_string(usedCount) + " block(s)";
        if (ImGui::Button(deleteLabel.c_str()))
        {
            for (size_t i = doc.blocks.size(); i-- > 0;)
            {
                if (doc.blocks[i].clipId == m_deleteClipId)
                {
                    doc.blocks.erase(doc.blocks.begin() + static_cast<long>(i));
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
