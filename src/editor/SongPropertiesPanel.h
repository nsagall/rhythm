#pragma once

#include <string>

class BlockPlayer;
struct EditorDocument;

// Formats seconds as "3:45" - never plain seconds. Shared with EditorApp's "Estimated length"
// readout so the two stay worded identically.
std::wstring FormatMinutesSeconds(double seconds);

// Editor panel for the [song] block's fields (title, tempo, time signature, default hit
// tolerances), as opposed to per-clip or per-block fields. Also surfaces the chart's estimated
// length under perfect play - computed, not stored.
class SongPropertiesPanel
{
public:
    // Draws the panel into whatever ImGui window the caller has begun. Mutates doc on any edit.
    // player supplies the current BlockSchedule (via CurrentSchedule()) for the estimated-length
    // readout.
    void Draw(EditorDocument& doc, BlockPlayer& player);

    // Forces the title edit buffer to re-sync from doc on the next Draw() - call after anything
    // that replaces doc's contents (New/Open, undo/redo).
    void NotifyDocumentReplaced()
    {
        m_titleSynced = false;
    }

private:
    bool m_titleSynced = false;
    char m_titleBuf[256] = {};
};
