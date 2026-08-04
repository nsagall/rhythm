#pragma once

#include <imgui.h>

// A thin drag handle for resizing the fixed, manually-positioned panes
// EditorApp lays out each frame (see EditorApp.cpp's Update()) - this
// editor doesn't use Dear ImGui's docking branch, so there's no built-in
// splitter between separately-Begin'd windows. Draws its own borderless
// window at an absolute screen position; also works to split two child
// regions inside a single window, since SetNextWindowPos always takes
// absolute screen coordinates regardless of what's currently open.
//
// barPos/barSize describe the handle's rectangle in screen space (make
// barSize's thickness dimension a few pixels, e.g. 6). resizesVertically
// selects both the drag axis and cursor shape: true for a horizontal bar
// the user drags up/down (splitting two stacked panes), false for a
// vertical bar dragged left/right (splitting two side-by-side panes).
//
// Returns the signed pixel delta the mouse moved along that axis this
// frame while the handle is held down (0 if not currently being dragged) -
// the caller decides how to distribute that delta between whichever pane
// sizes it owns (see EditorApp::Update()'s callers for the two patterns:
// growing one stored pane at a neighboring remainder's expense, or
// transferring pixels between two independently-stored panes).
float DrawPaneSplitter(const char* id, ImVec2 barPos, ImVec2 barSize, bool resizesVertically);
