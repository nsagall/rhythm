#include "SectionPanel.h"

#include <imgui.h>

#include <utility>

#include "Utf8.h"

namespace
{

const char* kKindNames[] = {"Learn", "Break", "Reset", "Background"};

ImVec4 KindColor(SectionKind kind)
{
    switch (kind)
    {
        case SectionKind::Learn:
            return ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
        case SectionKind::Break:
            return ImVec4(1.0f, 0.6f, 0.25f, 1.0f);
        case SectionKind::Reset:
            return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        case SectionKind::Background:
            return ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void SectionPanel::Draw(EditorDocument& doc, PreviewPlayer* previewPlayer)
{
    if (ImGui::Button("Add Section"))
    {
        ImGui::OpenPopup("AddSectionPopup");
    }
    if (ImGui::BeginPopup("AddSectionPopup"))
    {
        for (int i = 0; i < 4; ++i)
        {
            if (ImGui::Selectable(kKindNames[i]))
            {
                EditorSection section;
                section.id = doc.nextSectionId++;
                section.kind = static_cast<SectionKind>(i);
                section.clipId = -1;
                section.loopCount = 1;
                doc.sections.push_back(section);
                MarkDirty(doc);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::BeginChild("SectionList", ImVec2(0, 0), true);
    for (size_t i = 0; i < doc.sections.size();)
    {
        if (DrawRow(doc, previewPlayer, i))
        {
            continue; // this row deleted itself; doc.sections has shifted, don't advance i
        }
        ++i;
    }
    ImGui::EndChild();
}

bool SectionPanel::DrawRow(EditorDocument& doc, PreviewPlayer* previewPlayer, size_t index)
{
    EditorSection& section = doc.sections[index];
    ImGui::PushID(section.id);

    bool isCurrent = previewPlayer != nullptr && previewPlayer->CurrentSectionIndex() == static_cast<int>(index);
    ImGui::Text(isCurrent ? ">" : " ");
    ImGui::SameLine();

    ImGui::TextColored(KindColor(section.kind), "%d.", static_cast<int>(index) + 1);
    ImGui::SameLine();

    int kindIndex = static_cast<int>(section.kind);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("##kind", &kindIndex, kKindNames, 4))
    {
        section.kind = static_cast<SectionKind>(kindIndex);
        if (section.kind == SectionKind::Reset)
        {
            section.clipId = -1;
            section.loopCount = 1;
        }
        MarkDirty(doc);
    }

    if (section.kind != SectionKind::Reset)
    {
        ImGui::SameLine();
        const EditorClip* currentClip = FindClipById(doc, section.clipId);
        std::string previewLabel = currentClip != nullptr ? ToUtf8(currentClip->displayName) : "(choose clip)";
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##clip", previewLabel.c_str()))
        {
            for (const EditorClip& clip : doc.clips)
            {
                bool selected = clip.id == section.clipId;
                if (ImGui::Selectable(ToUtf8(clip.displayName).c_str(), selected))
                {
                    section.clipId = clip.id;
                    MarkDirty(doc);
                }
            }
            ImGui::EndCombo();
        }
        if (section.clipId < 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "required");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        int loopCount = section.loopCount;
        if (ImGui::InputInt("loops", &loopCount))
        {
            if (loopCount < 1)
            {
                loopCount = 1;
            }
            section.loopCount = loopCount;
            MarkDirty(doc);
        }
    }

    ImGui::SameLine();
    if (index == 0)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::ArrowButton("##up", ImGuiDir_Up))
    {
        std::swap(doc.sections[index], doc.sections[index - 1]);
        MarkDirty(doc);
    }
    if (index == 0)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    bool isLast = index + 1 >= doc.sections.size();
    if (isLast)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::ArrowButton("##down", ImGuiDir_Down))
    {
        std::swap(doc.sections[index], doc.sections[index + 1]);
        MarkDirty(doc);
    }
    if (isLast)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    bool canSeek = previewPlayer != nullptr && (previewPlayer->IsPlaying() || previewPlayer->IsPaused());
    if (!canSeek)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Seek"))
    {
        previewPlayer->SeekToSection(static_cast<int>(index));
    }
    if (!canSeek)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    bool deleted = false;
    if (ImGui::Button("X"))
    {
        doc.sections.erase(doc.sections.begin() + static_cast<long>(index));
        MarkDirty(doc);
        deleted = true;
    }

    ImGui::PopID();
    return deleted;
}
