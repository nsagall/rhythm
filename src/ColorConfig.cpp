#include "ColorConfig.h"

#include "Colors.h"

#include <cwchar>

namespace Colors
{
namespace
{

// section/name are ASCII-only by construction (see Entry's own comment) -
// a plain byte-widen is exact, no codepage/UTF-8 decoding needed.
std::wstring WidenAscii(const char* s)
{
    std::wstring out;
    for (const char* p = s; *p != '\0'; ++p)
    {
        out.push_back(static_cast<wchar_t>(*p));
    }
    return out;
}

} // namespace

const std::vector<Entry>& AllEntries()
{
    // clang-format off
    static const std::vector<Entry> entries = {
        {"ClipColor", "Neutral", &ClipColor::kNeutral},
        {"ClipColor", "Palette0", &ClipColor::kPalette[0]},
        {"ClipColor", "Palette1", &ClipColor::kPalette[1]},
        {"ClipColor", "Palette2", &ClipColor::kPalette[2]},
        {"ClipColor", "Palette3", &ClipColor::kPalette[3]},
        {"ClipColor", "Palette4", &ClipColor::kPalette[4]},
        {"ClipColor", "Palette5", &ClipColor::kPalette[5]},
        {"ClipColor", "Palette6", &ClipColor::kPalette[6]},
        {"ClipColor", "Palette7", &ClipColor::kPalette[7]},

        {"GameColors", "BgTop", &GameColors::kBgTop},
        {"GameColors", "BgBottom", &GameColors::kBgBottom},
        {"GameColors", "BorderColor", &GameColors::kBorderColor},
        {"GameColors", "TextColor", &GameColors::kTextColor},
        {"GameColors", "StreakColor", &GameColors::kStreakColor},
        {"GameColors", "PanelBgColor", &GameColors::kPanelBgColor},
        {"GameColors", "ShadowColor", &GameColors::kShadowColor},
        {"GameColors", "HighlightWhite", &GameColors::kHighlightWhite},
        {"GameColors", "NoteColorHit", &GameColors::kNoteColorHit},
        {"GameColors", "NoteColorMiss", &GameColors::kNoteColorMiss},
        {"GameColors", "NoteColorHitImprecise", &GameColors::kNoteColorHitImprecise},
        {"GameColors", "ConfettiPalette0", &GameColors::kConfettiPalette[0]},
        {"GameColors", "ConfettiPalette1", &GameColors::kConfettiPalette[1]},
        {"GameColors", "ConfettiPalette2", &GameColors::kConfettiPalette[2]},
        {"GameColors", "ConfettiPalette3", &GameColors::kConfettiPalette[3]},
        {"GameColors", "ConfettiPalette4", &GameColors::kConfettiPalette[4]},
        {"GameColors", "ConfettiPalette5", &GameColors::kConfettiPalette[5]},
        {"GameColors", "ScorePopupBankedColor", &GameColors::kScorePopupBankedColor},
        {"GameColors", "WindowBgColor", &GameColors::kWindowBgColor},
        {"GameColors", "FieldBgColor", &GameColors::kFieldBgColor},
        {"GameColors", "LabelTextColor", &GameColors::kLabelTextColor},
        {"GameColors", "RefreshButtonColor", &GameColors::kRefreshButtonColor},
        {"GameColors", "AssignButtonColor", &GameColors::kAssignButtonColor},
        {"GameColors", "SongRowHighlightColor", &GameColors::kSongRowHighlightColor},
        {"GameColors", "SongRowHighlightTextColor", &GameColors::kSongRowHighlightTextColor},
        {"GameColors", "HintTextColor", &GameColors::kHintTextColor},
        {"GameColors", "ToggleTrackOffColor", &GameColors::kToggleTrackOffColor},
        {"GameColors", "ToggleTrackOnColor", &GameColors::kToggleTrackOnColor},
        {"GameColors", "ToggleKnobColor", &GameColors::kToggleKnobColor},
    };
    // clang-format on
    return entries;
}

std::wstring ConfigFilePath()
{
    // GetPrivateProfileStringW/WritePrivateProfileStringW silently treat a
    // bare relative filename as relative to the *Windows* directory, not
    // the process's working directory - so this resolves the intended
    // "next to Content/" path explicitly rather than relying on a
    // relative "Colors.ini" (which would quietly read/write %WINDIR%
    // instead of the repo root).
    wchar_t currentDir[MAX_PATH] = L"";
    GetCurrentDirectoryW(MAX_PATH, currentDir);
    return std::wstring(currentDir) + L"\\Colors.ini";
}

void LoadFromIni()
{
    std::wstring path = ConfigFilePath();
    for (const Entry& entry : AllEntries())
    {
        std::wstring section = WidenAscii(entry.section);
        std::wstring name = WidenAscii(entry.name);

        wchar_t buffer[32] = L"";
        GetPrivateProfileStringW(section.c_str(), name.c_str(), L"", buffer, 32, path.c_str());
        if (buffer[0] == L'\0')
        {
            continue; // no saved key - leave Colors.h's compiled-in default in place
        }

        int r = 0;
        int g = 0;
        int b = 0;
        if (swscanf(buffer, L"%d,%d,%d", &r, &g, &b) == 3)
        {
            *entry.value = RGB(r, g, b);
        }
    }
}

void SaveToIni()
{
    std::wstring path = ConfigFilePath();
    for (const Entry& entry : AllEntries())
    {
        std::wstring section = WidenAscii(entry.section);
        std::wstring name = WidenAscii(entry.name);

        wchar_t buffer[32];
        swprintf(buffer, 32, L"%d,%d,%d", GetRValue(*entry.value), GetGValue(*entry.value), GetBValue(*entry.value));
        WritePrivateProfileStringW(section.c_str(), name.c_str(), buffer, path.c_str());
    }
}

} // namespace Colors
