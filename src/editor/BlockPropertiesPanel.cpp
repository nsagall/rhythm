#include "BlockPropertiesPanel.h"

#include <imgui.h>

#include "Utf8.h"

namespace
{
const char* kKindNames[] = {"Learn", "Break", "Reset", "Background"};
} // namespace

bool BlockPropertiesPanel::Draw(EditorDocument& doc, int selectedBlockId, BlockPlayer& player)
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
        return false;
    }

    int kindIndex = static_cast<int>(block->kind);
    if (ImGui::Combo("Kind", &kindIndex, kKindNames, 4))
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
        std::string previewLabel = currentClip != nullptr ? ToUtf8(currentClip->displayName) : "(choose clip)";
        if (ImGui::BeginCombo("Clip", previewLabel.c_str()))
        {
            for (const EditorClip& clip : doc.clips)
            {
                bool selected = clip.id == block->clipId;
                if (ImGui::Selectable(ToUtf8(clip.displayName).c_str(), selected))
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
            if (entry->lockInSeconds >= 0.0)
            {
                ImGui::Text("Locks in at %.2fs into this block's audio (glow/confetti/volume-switch only - the",
                             entry->lockInSeconds - entry->sectionStartSeconds);
                ImGui::Text("block's own duration above doesn't depend on it)");
            }
            else
            {
                ImGui::TextDisabled("Never locks in a perfect player's hits_required before this block ends");
            }
        }
        ImGui::Text("Total: %.2fs", entry->endSeconds - entry->sectionStartSeconds);
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
    bool deleted = false;
    if (ImGui::Button("Delete Block"))
    {
        doc.blocks.erase(doc.blocks.begin() + blockIndex);
        MarkDirty(doc);
        deleted = true;
    }
    return deleted;
}
