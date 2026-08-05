#include "BlockTimeline.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "Utf8.h"

namespace
{

// Narrow by design - the point is to see as much of the song's structure
// at once as possible; a block only needs to be wide enough to read its
// label and to be a comfortable click/drag target, not to be visually
// proportioned like a duration bar.
constexpr float kPixelsPerSecond = 22.0f;
constexpr float kMinLearnBreakWidthPx = 34.0f;
constexpr float kDefaultLearnBreakWidthPx = 56.0f; // used when no schedule/entry is available yet
constexpr float kMarkerWidthPx = 14.0f;            // Background/Reset - narrow, reads as "doesn't take real time"
constexpr float kBlockHeight = 64.0f;
constexpr float kBlockGap = 2.0f;
constexpr float kPlayheadStripHeight = 14.0f; // room above the blocks for the playhead's triangle marker
constexpr float kLabelFontScale = 1.2f;       // "a little larger" than the default UI text

const char* kKindNames[] = {"Learn", "Break", "Reset", "Background"};

// The default ImGui font (no bundled bold weight) has no true bold variant
// to switch to, so bold is faked the standard bitmap-font way: draw the
// text twice, offset by a single pixel, so the strokes double up and read
// noticeably heavier without needing a second font asset.
void DrawBoldText(ImDrawList* drawList, ImVec2 pos, ImU32 color, const char* text)
{
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize() * kLabelFontScale;
    drawList->AddText(font, fontSize, pos, color, text);
    drawList->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y), color, text);
}

// isDontFailLearn is only ever true for a Learn block whose clip declared
// learn_mode = dontfail (see ChartFile.h's LearnMode) - darkening it a
// little is enough to tell it apart from a Pass Learn block at a glance,
// without needing a legend or crowding the block's own label.
ImVec4 BlockKindColor(SectionKind kind, bool isDontFailLearn)
{
    ImVec4 color;
    switch (kind)
    {
        case SectionKind::Learn:
            color = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
            break;
        case SectionKind::Break:
            color = ImVec4(1.0f, 0.6f, 0.25f, 1.0f);
            break;
        case SectionKind::Reset:
            color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            break;
        case SectionKind::Background:
            color = ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
            break;
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (isDontFailLearn)
    {
        color.x *= 0.65f;
        color.y *= 0.65f;
        color.z *= 0.65f;
    }
    return color;
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

    DrawSeekRuler(player, layout, origin.x, origin.y);

    float totalWidth = layout.empty() ? 0.0f : (layout.back().leftX + layout.back().width);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + kBlockHeight));
    ImGui::Dummy(ImVec2(std::max(totalWidth, 1.0f), 1.0f));

    // Delete key removes the selected block without needing to reach for
    // BlockPropertiesPanel's own Delete Block button - only while this
    // scroll region itself has focus (i.e. the user just clicked a block
    // here), and never while a text field elsewhere wants the keystroke.
    if (m_selectedBlockId >= 0 && ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        for (size_t i = 0; i < doc.blocks.size(); ++i)
        {
            if (doc.blocks[i].id == m_selectedBlockId)
            {
                doc.blocks.erase(doc.blocks.begin() + static_cast<long>(i));
                MarkDirty(doc);
                m_selectedBlockId = -1;
                break;
            }
        }
    }

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
        bool isDontFailLearn = block.kind == SectionKind::Learn && clip != nullptr && clip->learnMode == LearnMode::DontFail;

        ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(BlockKindColor(block.kind, isDontFailLearn));
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
        DrawBoldText(drawList, ImVec2(rectMin.x + 4.0f, rectMin.y + 4.0f), IM_COL32(10, 10, 10, 255), label.c_str());
        drawList->PopClipRect();

        if (isDontFailLearn && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Learn Mode: Don't Fail (darker fill) - a miss drops the streak\nback to failing instead of locking in permanently.");
        }

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

void BlockTimeline::DrawSeekRuler(BlockPlayer& player, const std::vector<BlockLayout>& layout, float originX,
                                   float originY)
{
    const BlockSchedule::Schedule* schedule = player.CurrentSchedule();
    if (schedule == nullptr || schedule->entries.empty() || layout.empty())
    {
        return;
    }

    float totalWidth = layout.back().leftX + layout.back().width;

    ImGui::SetCursorScreenPos(ImVec2(originX, originY - kPlayheadStripHeight));
    ImGui::InvisibleButton("seekRuler", ImVec2(std::max(totalWidth, 1.0f), kPlayheadStripHeight));

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        float x = ImGui::GetMousePos().x - originX;
        player.SeekToSeconds(LayoutXToSeconds(x, layout, *schedule));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Click or drag to jump playback to this point.");
    }
}

double BlockTimeline::LayoutXToSeconds(float x, const std::vector<BlockLayout>& layout,
                                        const BlockSchedule::Schedule& schedule) const
{
    if (layout.empty())
    {
        return 0.0;
    }
    if (x <= 0.0f)
    {
        return 0.0;
    }
    float totalWidth = layout.back().leftX + layout.back().width;
    if (x >= totalWidth)
    {
        return schedule.totalSeconds;
    }

    for (size_t i = 0; i < layout.size(); ++i)
    {
        const BlockLayout& block = layout[i];
        if (x >= block.leftX + block.width)
        {
            continue;
        }

        if (block.entry != nullptr)
        {
            float frac = (x - block.leftX) / block.width;
            if (frac < 0.0f)
            {
                frac = 0.0f;
            }
            if (frac > 1.0f)
            {
                frac = 1.0f;
            }
            // Maps into the block's first loop pass only - loop 2+ of a
            // multi-loop block isn't individually reachable by clicking
            // its (single, fixed-width) rendered span, same tradeoff
            // BlockPlayer::SeekToBlockStart makes by always landing on
            // sectionStartSeconds rather than a specific pass. Pass 1's own
            // real duration - not necessarily the full loopSeconds the
            // block's width represents - mirrors Seek()'s own pass-1
            // handling exactly (see SeekResult::loopIndex's comment): the
            // real, phase-seeked audio voice can wrap sooner than a full
            // loopSeconds after audioStartSeconds, so the right edge of the
            // block (frac == 1.0) must map to that real, possibly-earlier
            // wrap instant, not to audioStartSeconds + loopSeconds - or a
            // click near the right edge could land past this entry's own
            // end, in whatever the next entry happens to be.
            double loopSeconds = block.entry->loopSeconds;
            double startPhase =
                loopSeconds > 0.0
                    ? std::fmod(block.entry->audioStartSeconds - block.entry->originSeconds, loopSeconds)
                    : 0.0;
            double firstPassSeconds = loopSeconds - startPhase;
            if (firstPassSeconds <= 1e-9)
            {
                firstPassSeconds = loopSeconds;
            }
            return block.entry->audioStartSeconds + static_cast<double>(frac) * firstPassSeconds;
        }

        // Background/Reset marker - zero real duration, so there's no
        // "position within it" to map; land on wherever this block's own
        // instant actually falls, exactly mirroring
        // BlockPlayer::SeekToBlockStart's own fallback for the same case.
        for (const BlockSchedule::Entry& entry : schedule.entries)
        {
            if (entry.sectionIndex >= static_cast<int>(i))
            {
                return entry.sectionStartSeconds;
            }
        }
        return schedule.totalSeconds;
    }

    return schedule.totalSeconds;
}
