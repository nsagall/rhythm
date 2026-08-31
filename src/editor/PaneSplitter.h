#pragma once

#include <imgui.h>

// A thin drag handle for resizing the manually-positioned panes EditorApp lays out each frame -
// this editor doesn't use ImGui's docking branch, so there's no built-in splitter. Draws its own
// borderless window at an absolute screen position.
//   id               - unique ImGui id for the handle.
//   barPos, barSize  - the handle's rectangle in screen space; make the thickness a few pixels.
//   resizesVertically - true for a horizontal bar dragged up/down (stacked panes), false for a
//                       vertical bar dragged left/right (side-by-side panes). Sets both the drag
//                       axis and cursor shape.
// Returns the signed pixel delta the mouse moved along that axis this frame while held (0 if not
// being dragged); the caller distributes it among the pane sizes it owns.
float DrawPaneSplitter(const char* id, ImVec2 barPos, ImVec2 barSize, bool resizesVertically);
