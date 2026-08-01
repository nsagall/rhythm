#include "BlockTimeline.h"

#include <algorithm>

#include <imgui.h>

#include "Utf8.h"

namespace
{

constexpr float kPixelsPerSecond = 60.0f;
constexpr float kMinLearnBreakWidthPx = 60.0f;
constexpr float kDefaultLearnBreakWidthPx = 140.0f; // used when no schedule/entry is available yet
constexpr float kMarkerWidthPx = 28.0f;             // Background/Reset - narrow, reads as "doesn't take real time"
constexpr float kBlockHeight = 64.0f;
constexpr float kBlockGap = 4.0f;
constexpr float kPlayheadStripHeight = 14.0f; // room above the blocks for the playhead's triangle marker

const char* kKindNames[] = {"Learn", "Break", "Reset", "Background"};

ImVec4 BlockKindColor(SectionKind kind)
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

void BlockTimeline::Draw(EditorDocument& doc, BlockPlayer& player)
{
    DrawToolbar(doc);
    ImGui::Separator();

    const BlockSchedule::Schedule* schedule = player.CurrentSchedule();
    std::vector<BlockLayout> layout = ComputeLayout(doc, schedule);

    ImGui::BeginChild("BlockTimelineScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.y += kPlayheadStripHeight;

    DrawBlockRow(doc, layout, origin.x, origin.y);

    if (schedule != nullptr && !schedule->entries.empty())
    {
        DrawPlayhead(*schedule, player.PositionSeconds(), layout, origin.x, origin.y);
    }

    float totalWidth = layout.empty() ? 0.0f : (layout.back().leftX + layout.back().width);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + kBlockHeight));
    ImGui::Dummy(ImVec2(std::max(totalWidth, 1.0f), 1.0f));

    ImGui::EndChild();
}

void BlockTimeline::DrawToolbar(EditorDocument& doc)
{
    if (ImGui::Button("Add Block"))
    {
        ImGui::OpenPopup("AddBlockPopup");
    }
    if (ImGui::BeginPopup("AddBlockPopup"))
    {
        for (int k = 0; k < 4; ++k)
        {
            if (ImGui::Selectable(kKindNames[k]))
            {
                EditorBlock block;
                block.id = doc.nextBlockId++;
                block.kind = static_cast<SectionKind>(k);
                block.clipId = -1;
                block.loopCount = 1;
                doc.blocks.push_back(block);
                m_selectedBlockId = block.id;
                MarkDirty(doc);
            }
        }
        ImGui::EndPopup();
    }
}

std::vector<BlockTimeline::BlockLayout> BlockTimeline::ComputeLayout(const EditorDocument& doc,
                                                                       const BlockSchedule::Schedule* schedule) const
{
    std::vector<BlockLayout> layout(doc.blocks.size());
    float cursorX = 0.0f;
    for (size_t i = 0; i < doc.blocks.size(); ++i)
    {
        const EditorBlock& block = doc.blocks[i];
        const BlockSchedule::Entry* entry = nullptr;
        if (schedule != nullptr && (block.kind == SectionKind::Learn || block.kind == SectionKind::Break))
        {
            for (const BlockSchedule::Entry& e : schedule->entries)
            {
                if (e.sectionIndex == static_cast<int>(i))
                {
                    entry = &e;
                    break;
                }
            }
        }

        float width;
        if (block.kind == SectionKind::Background || block.kind == SectionKind::Reset)
        {
            width = kMarkerWidthPx;
        }
        else if (entry != nullptr)
        {
            width = std::max(kMinLearnBreakWidthPx, static_cast<float>(entry->loopSeconds) * kPixelsPerSecond);
        }
        else
        {
            width = kDefaultLearnBreakWidthPx;
        }

        layout[i].leftX = cursorX;
        layout[i].width = width;
        layout[i].entry = entry;
        cursorX += width + kBlockGap;
    }
    return layout;
}

void BlockTimeline::DrawBlockRow(EditorDocument& doc, const std::vector<BlockLayout>& layout, float originX,
                                  float originY)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    for (size_t i = 0; i < doc.blocks.size(); ++i)
    {
        EditorBlock& block = doc.blocks[i];
        ImGui::PushID(block.id);
        ImGui::SetCursorScreenPos(ImVec2(originX + layout[i].leftX, originY));
        ImGui::InvisibleButton("block", ImVec2(layout[i].width, kBlockHeight));

        if (ImGui::IsItemClicked())
        {
            m_selectedBlockId = block.id;
        }

        if (ImGui::BeginDragDropSource())
        {
            int sourceIndex = static_cast<int>(i);
            ImGui::SetDragDropPayload("BLOCK_REORDER", &sourceIndex, sizeof(int));
            const EditorClip* dragClip = FindClipById(doc, block.clipId);
            std::string dragLabel = dragClip != nullptr ? ToUtf8(dragClip->displayName) : kKindNames[static_cast<int>(block.kind)];
            ImGui::Text("%s", dragLabel.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BLOCK_REORDER"))
            {
                int sourceIndex = *static_cast<const int*>(payload->Data);
                int targetIndex = static_cast<int>(i);
                if (sourceIndex != targetIndex)
                {
                    EditorBlock moved = doc.blocks[static_cast<size_t>(sourceIndex)];
                    doc.blocks.erase(doc.blocks.begin() + sourceIndex);
                    if (sourceIndex < targetIndex)
                    {
                        --targetIndex; // erase already shifted everything after sourceIndex back by one
                    }
                    doc.blocks.insert(doc.blocks.begin() + targetIndex, moved);
                    MarkDirty(doc);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 rectMax = ImGui::GetItemRectMax();

        const EditorClip* clip = FindClipById(doc, block.clipId);
        bool missingClip = block.kind != SectionKind::Reset && clip == nullptr;

        ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(BlockKindColor(block.kind));
        drawList->AddRectFilled(rectMin, rectMax, fillColor, 4.0f);
        if (missingClip)
        {
            drawList->AddRect(rectMin, rectMax, IM_COL32(255, 40, 40, 255), 4.0f, 0, 2.0f);
        }
        if (block.id == m_selectedBlockId)
        {
            drawList->AddRect(rectMin, rectMax, IM_COL32(255, 255, 255, 255), 4.0f, 0, 2.0f);
        }

        std::string label = clip != nullptr ? ToUtf8(clip->displayName) : kKindNames[static_cast<int>(block.kind)];
        drawList->PushClipRect(rectMin, rectMax, true);
        drawList->AddText(ImVec2(rectMin.x + 6.0f, rectMin.y + 6.0f), IM_COL32(10, 10, 10, 255), label.c_str());
        drawList->PopClipRect();

        ImGui::PopID();
    }
}

void BlockTimeline::DrawPlayhead(const BlockSchedule::Schedule& schedule, double playheadSeconds,
                                  const std::vector<BlockLayout>& layout, float originX, float originY)
{
    BlockSchedule::SeekResult result = BlockSchedule::Seek(schedule, playheadSeconds);
    if (result.entryIndex < 0)
    {
        return;
    }
    const BlockSchedule::Entry& entry = schedule.entries[static_cast<size_t>(result.entryIndex)];
    size_t blockIndex = static_cast<size_t>(entry.sectionIndex);
    if (blockIndex >= layout.size())
    {
        return;
    }

    const BlockLayout& block = layout[blockIndex];
    float frac = entry.loopSeconds > 0.0 ? static_cast<float>(result.phaseSeconds / entry.loopSeconds) : 0.0f;
    float x = originX + block.leftX + frac * block.width;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr ImU32 kPlayheadColor = IM_COL32(255, 240, 60, 255);
    drawList->AddLine(ImVec2(x, originY), ImVec2(x, originY + kBlockHeight), kPlayheadColor, 3.0f);
    drawList->AddTriangleFilled(ImVec2(x - 6.0f, originY - kPlayheadStripHeight), ImVec2(x + 6.0f, originY - kPlayheadStripHeight),
                                 ImVec2(x, originY), kPlayheadColor);
}
