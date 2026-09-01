#include "SongPropertiesPanel.h"

#include <imgui.h>

#include <cstring>
#include <cwchar>

#include "BlockPlayer.h"
#include "EditorDocument.h"
#include "Utf8.h"

std::wstring FormatMinutesSeconds(double seconds)
{
    int totalSeconds = static_cast<int>(seconds + 0.5);
    if (totalSeconds < 0)
    {
        totalSeconds = 0;
    }
    int minutes = totalSeconds / 60;
    int secs = totalSeconds % 60;
    wchar_t buf[32];
    swprintf(buf, 32, L"%d:%02d", minutes, secs);
    return buf;
}

void SongPropertiesPanel::Draw(EditorDocument& doc, BlockPlayer& player)
{
    if (!m_titleSynced)
    {
        std::string title = ToUtf8(doc.title);
        size_t len = title.size() < sizeof(m_titleBuf) - 1 ? title.size() : sizeof(m_titleBuf) - 1;
        memcpy(m_titleBuf, title.data(), len);
        m_titleBuf[len] = '\0';
        m_titleSynced = true;
    }

    ImGui::Text("Title");
    if (ImGui::InputText("##songTitle", m_titleBuf, sizeof(m_titleBuf)))
    {
        doc.title = FromUtf8(m_titleBuf);
        MarkDirty(doc);
    }
    if (m_titleBuf[0] == '\0')
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "title is required");
    }

    ImGui::Separator();

    float bpm = static_cast<float>(doc.bpm);
    if (ImGui::InputFloat("BPM", &bpm, 0.0f, 0.0f, "%.2f"))
    {
        doc.bpm = bpm > 0.0f ? static_cast<double>(bpm) : 0.01;
        MarkDirty(doc);
    }

    ImGui::Text("Time Signature");
    int beatsPerBar = doc.beatsPerBar;
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::InputInt("##tsNumerator", &beatsPerBar))
    {
        if (beatsPerBar < 1)
        {
            beatsPerBar = 1;
        }
        doc.beatsPerBar = beatsPerBar;
        MarkDirty(doc);
    }
    ImGui::SameLine();
    ImGui::Text("/");
    ImGui::SameLine();
    int denominator = doc.timeSignatureDenominator;
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::InputInt("##tsDenominator", &denominator))
    {
        if (denominator < 1)
        {
            denominator = 1;
        }
        doc.timeSignatureDenominator = denominator;
        MarkDirty(doc);
    }

    ImGui::Separator();
    const BlockSchedule::Schedule* schedule = player.CurrentSchedule();
    if (schedule != nullptr && !schedule->entries.empty())
    {
        // Assumes perfect learning - the same assumption the whole block
        // schedule/playhead is built on (see BlockSchedule.h).
        ImGui::Text("Estimated Length (perfect play): %s", ToUtf8(FormatMinutesSeconds(schedule->totalSeconds)).c_str());
    }
    else
    {
        ImGui::TextDisabled("Estimated Length: (unavailable - fix validation problems first)");
    }

    ImGui::Separator();
    ImGui::Text("Default Timing Tolerances");
    ImGui::TextDisabled("Applies to every clip that doesn't override it.");

    float startTol = static_cast<float>(doc.startToleranceMs);
    if (ImGui::InputFloat("Start Tolerance (ms)", &startTol, 0.0f, 0.0f, "%.1f"))
    {
        doc.startToleranceMs = startTol > 0.0f ? static_cast<double>(startTol) : 0.1;
        MarkDirty(doc);
    }

    float releaseTol = static_cast<float>(doc.releaseToleranceMs);
    if (ImGui::InputFloat("Release Tolerance (ms)", &releaseTol, 0.0f, 0.0f, "%.1f"))
    {
        doc.releaseToleranceMs = releaseTol > 0.0f ? static_cast<double>(releaseTol) : 0.1;
        MarkDirty(doc);
    }
}
