#include "PaneSplitter.h"

namespace
{
constexpr ImGuiWindowFlags c_SplitterFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
} // namespace

float DrawPaneSplitter(const char* id, ImVec2 barPos, ImVec2 barSize, bool resizesVertically)
{
    ImGui::SetNextWindowPos(barPos);
    ImGui::SetNextWindowSize(barSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(id, nullptr, c_SplitterFlags);

    ImGui::InvisibleButton("##handle", barSize);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    float delta = 0.0f;
    if (active)
    {
        delta = resizesVertically ? ImGui::GetIO().MouseDelta.y : ImGui::GetIO().MouseDelta.x;
    }
    if (hovered || active)
    {
        ImGui::SetMouseCursor(resizesVertically ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    }

    ImU32 color = active ? IM_COL32(96, 150, 230, 255) : hovered ? IM_COL32(90, 90, 100, 255) : IM_COL32(45, 45, 50, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), color);

    ImGui::End();
    ImGui::PopStyleVar();
    return delta;
}
