#include "ColorConfig.h"

#include "Colors.h"

#include <cwchar>

namespace Colors
{
namespace
{

// Widens an ASCII-only string byte-for-byte (no codepage/UTF-8 decoding needed).
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
        {"ClipColor", "Neutral", &ClipColor::c_Neutral},
        {"ClipColor", "Palette0", &ClipColor::c_Palette[0]},
        {"ClipColor", "Palette1", &ClipColor::c_Palette[1]},
        {"ClipColor", "Palette2", &ClipColor::c_Palette[2]},
        {"ClipColor", "Palette3", &ClipColor::c_Palette[3]},
        {"ClipColor", "Palette4", &ClipColor::c_Palette[4]},
        {"ClipColor", "Palette5", &ClipColor::c_Palette[5]},
        {"ClipColor", "Palette6", &ClipColor::c_Palette[6]},
        {"ClipColor", "Palette7", &ClipColor::c_Palette[7]},

        {"GameColors", "BgTop", &GameColors::c_BgTop},
        {"GameColors", "BgBottom", &GameColors::c_BgBottom},
        {"GameColors", "BorderColor", &GameColors::c_BorderColor},
        {"GameColors", "TextColor", &GameColors::c_TextColor},
        {"GameColors", "StreakColor", &GameColors::c_StreakColor},
        {"GameColors", "PanelBgColor", &GameColors::c_PanelBgColor},
        {"GameColors", "ShadowColor", &GameColors::c_ShadowColor},
        {"GameColors", "HighlightWhite", &GameColors::c_HighlightWhite},
        {"GameColors", "NoteColorHit", &GameColors::c_NoteColorHit},
        {"GameColors", "NoteColorMiss", &GameColors::c_NoteColorMiss},
        {"GameColors", "NoteColorHitImprecise", &GameColors::c_NoteColorHitImprecise},
        {"GameColors", "ConfettiPalette0", &GameColors::c_ConfettiPalette[0]},
        {"GameColors", "ConfettiPalette1", &GameColors::c_ConfettiPalette[1]},
        {"GameColors", "ConfettiPalette2", &GameColors::c_ConfettiPalette[2]},
        {"GameColors", "ConfettiPalette3", &GameColors::c_ConfettiPalette[3]},
        {"GameColors", "ConfettiPalette4", &GameColors::c_ConfettiPalette[4]},
        {"GameColors", "ConfettiPalette5", &GameColors::c_ConfettiPalette[5]},
        {"GameColors", "ScorePopupBankedColor", &GameColors::c_ScorePopupBankedColor},
        {"GameColors", "WindowBgColor", &GameColors::c_WindowBgColor},
        {"GameColors", "FieldBgColor", &GameColors::c_FieldBgColor},
        {"GameColors", "LabelTextColor", &GameColors::c_LabelTextColor},
        {"GameColors", "RefreshButtonColor", &GameColors::c_RefreshButtonColor},
        {"GameColors", "AssignButtonColor", &GameColors::c_AssignButtonColor},
        {"GameColors", "SongRowHighlightColor", &GameColors::c_SongRowHighlightColor},
        {"GameColors", "SongRowHighlightTextColor", &GameColors::c_SongRowHighlightTextColor},
        {"GameColors", "HintTextColor", &GameColors::c_HintTextColor},
        {"GameColors", "ToggleTrackOffColor", &GameColors::c_ToggleTrackOffColor},
        {"GameColors", "ToggleTrackOnColor", &GameColors::c_ToggleTrackOnColor},
        {"GameColors", "ToggleKnobColor", &GameColors::c_ToggleKnobColor},
    };
    // clang-format on
    return entries;
}

std::wstring ConfigFilePath()
{
    // GetPrivateProfileStringW/WritePrivateProfileStringW resolve a bare relative filename against
    // the Windows directory, not the working directory - so build an absolute path explicitly.
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
