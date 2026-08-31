#include "BlockPropertiesPanel.h"

#include <imgui.h>

#include "Utf8.h"

namespace
{
const char* c_KindNames[] = {"Learn", "Break", "Reset", "Background"};
} // namespace

int BlockPropertiesPanel::Draw(EditorDocument& doc, int selectedBlockId, BlockPlayer& player)
{
    EditorBlock* block = nullptr;
    int blockIndex = -1;
    for (size_t i = 0; i < doc.blocks.size(); ++i)
    {
        if (doc.blocks[i].id == selectedBlockId)
        {
            block = &doc.blocks[i];
            blockIndex = static_cast<int>(i);
            break;
        }
    }

    if (block == nullptr)
    {
        ImGui::TextDisabled("Select a block on the timeline below.");
        return selectedBlockId;
    }

    int kindIndex = static_cast<int>(block->kind);
    if (ImGui::Combo("Kind", &kindIndex, c_KindNames, 4))
    {
        block->kind = static_cast<SectionKind>(kindIndex);
        if (block->kind == SectionKind::Reset)
        {
            block->clipId = -1;
            block->loopCount = 1;
        }
        MarkDirty(doc);
    }

    if (block->kind != SectionKind::Reset)
    {
        const EditorClip* currentClip = FindClipById(doc, block->clipId);
        std::string previewLabel = currentClip != nullptr ? ToUtf8(currentClip->name) : "(choose clip)";
        if (ImGui::BeginCombo("Clip", previewLabel.c_str()))
        {
            for (const EditorClip& clip : doc.clips)
            {
                bool selected = clip.id == block->clipId;
                if (ImGui::Selectable(ToUtf8(clip.name).c_str(), selected))
                {
                    block->clipId = clip.id;
                    MarkDirty(doc);
                }
            }
            ImGui::EndCombo();
        }
        if (block->clipId < 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "A clip is required.");
        }

        bool loopCountMeaningless = block->kind == SectionKind::Background;
        if (loopCountMeaningless)
        {
            ImGui::BeginDisabled();
        }
        int loopCount = block->loopCount;
        if (ImGui::InputInt("Loop Count", &loopCount))
        {
            if (loopCount < 1)
            {
                loopCount = 1;
            }
            block->loopCount = loopCount;
            MarkDirty(doc);
        }
        if (loopCountMeaningless)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("loop_count has no effect on a Background block - it loops until the next\nBreak or Reset regardless.");
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Reset has no clip or loop count - it stops everything and\nadvances immediately.");
    }

    ImGui::Separator();
    ImGui::Text("Computed timing");
    const BlockSchedule::Schedule* schedule = player.CurrentSchedule();
    const BlockSchedule::Entry* entry = nullptr;
    if (schedule != nullptr)
    {
        for (const BlockSchedule::Entry& e : schedule->entries)
        {
            if (e.sectionIndex == blockIndex)
            {
                entry = &e;
                break;
            }
        }
    }
    if (entry != nullptr)
    {
        ImGui::Text("Duration: %.2fs per loop x %d loop(s)", entry->loopSeconds, entry->loopCount);
        if (entry->kind == SectionKind::Learn)
        {
            ImGui::Text("Locks in at %.2fs into this block's audio (glow/confetti/volume-switch only - the",
                         entry->lockInSeconds - entry->sectionStartSeconds);
            ImGui::Text("duration above already includes as many loops as it takes to reach it)");
        }
        ImGui::Text("Total: %.2fs", entry->endSeconds - entry->sectionStartSeconds);
    }
    else if (block->kind == SectionKind::Reset)
    {
        // Not a validation problem - Reset gets no BlockSchedule::Entry (it advances instantly,
        // contributing zero elapsed time), so there's nothing to compute.
        ImGui::TextDisabled("Reset has no duration of its own - it stops everything instantly\nand advances immediately to the next block.");
    }
    else if (block->kind == SectionKind::Background)
    {
        // Same as Reset above - Background never gets its own Entry either,
        // since it plays continuously alongside whichever Learn/Break is
        // current rather than occupying its own span of the timeline.
        ImGui::TextDisabled("Background has no timing entry of its own - it plays continuously\nunderneath whatever Learn/Break block is current, contributing no\nelapsed time to the timeline.");
    }
    else
    {
        ImGui::TextDisabled("(unavailable - fix validation problems to see computed timing)");
    }

    ImGui::Separator();
    bool canSeek = schedule != nullptr && !schedule->entries.empty();
    if (!canSeek)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Seek Here"))
    {
        player.SeekToBlockStart(blockIndex);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Jump playback to the moment this block begins.");
    }
    if (!canSeek)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Duplicate Block"))
    {
        EditorBlock copy = *block;
        copy.id = doc.nextBlockId++;
        doc.blocks.insert(doc.blocks.begin() + blockIndex + 1, copy);
        MarkDirty(doc);
        return copy.id;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Insert a copy of this block right after itself.");
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete Block"))
    {
        doc.blocks.erase(doc.blocks.begin() + blockIndex);
        MarkDirty(doc);
        return -1;
    }

    return selectedBlockId;
}
