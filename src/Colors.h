#pragma once

#include <windows.h>

// Single place for every hard-coded color used by Rhythm.exe. MainWindow's chrome and
// NoteLaneGdiRenderer's playfield/HUD both pull from here. RhythmEditor has its own ImGui theme.
//
// Every value below is a mutable `inline` global, not `constexpr` - the literal is the compiled-in
// default. Colors::LoadFromIni() overwrites whichever Colors.ini has a saved value for at startup,
// and ColorEditor.exe edits that same ini by writing through these globals' addresses (see
// Colors::AllEntries()).

// The accent color a clip's instance (NoteLaneScene.h's ClipPlaythrough) is identified by. Shared
// rather than renderer-private so every renderer gets the same answer for the same clip; a
// renderer wanting a different scheme can ignore ClipPlaythrough::color.
namespace ClipColor
{

// Shown for a clip that isn't currently relevant (Idle before a chart loads, or Complete) - a dim
// desaturated neutral, so an empty lane reads as "nothing playing".
inline COLORREF c_Neutral = RGB(150, 150, 170);

// One accent color per clip. A curated palette avoiding red/green/yellow so a clip's color is
// never mistaken for a hit/miss/imprecise note flash - kept to the teal-through-pink side of the
// wheel. Cycles (see ForIndex) if a chart has more clips than colors.
inline COLORREF c_Palette[] = {
    RGB(255, 70, 235),  // magenta
    RGB(56, 219, 255),  // electric cyan
    RGB(60, 225, 180),  // teal
    RGB(190, 140, 255), // violet
    RGB(70, 140, 255),  // azure
    RGB(255, 90, 170),  // hot pink
    RGB(215, 100, 255), // orchid
    RGB(130, 110, 255), // periwinkle
};
constexpr int c_PaletteSize = sizeof(c_Palette) / sizeof(c_Palette[0]);

// Returns the accent color for the clip at this position in
// ChartSong::clips, or c_Neutral for index < 0 (no clip).
inline COLORREF ForIndex(int index)
{
    if (index < 0)
    {
        return c_Neutral;
    }
    return c_Palette[static_cast<size_t>(index) % c_PaletteSize];
}

} // namespace ClipColor

// Explicit color constants for the playfield/HUD (NoteLaneGdiRenderer) and window chrome
// (MainWindow), both hand-painted with GDI rather than a system theme.
namespace GameColors
{

// --- Playfield background & general chrome (NoteLaneGdiRenderer) ---

// Deep indigo-to-violet backdrop - moody enough to make bright note colors
// pop, warm enough to not read as a cold "dev tool" dark mode.
inline COLORREF c_BgTop = RGB(46, 20, 92);
inline COLORREF c_BgBottom = RGB(15, 11, 42);
inline COLORREF c_BorderColor = RGB(120, 90, 190);
inline COLORREF c_TextColor = RGB(255, 250, 240);

// Also one of the confetti burst's colors.
inline COLORREF c_StreakColor = RGB(255, 205, 70);

// Translucent fill shared by the HUD, hits meter, and debug overlay panels. The alpha is a
// translucency amount, not a color, so it stays a compile-time constant rather than in Colors.ini.
inline COLORREF c_PanelBgColor = RGB(12, 8, 28);
constexpr BYTE c_PanelBgAlpha = 175;

// Black/white for drop shadows and specular highlights - never a clip/judgement color.
inline COLORREF c_ShadowColor = RGB(0, 0, 0);
inline COLORREF c_HighlightWhite = RGB(255, 255, 255);

// Pass/fail feedback: saturated high-contrast green/red so the judgement reads instantly.
// c_NoteColorHit doubles as the "correctly held, not yet released" color.
inline COLORREF c_NoteColorHit = RGB(40, 235, 80);
inline COLORREF c_NoteColorMiss = RGB(255, 30, 30);

// A hit whose press wasn't precise (still correct, just sloppy) - renders yellow instead of green,
// for its ripple and note block (via ColorForNote). Receptor/flash colors stay c_NoteColorHit.
inline COLORREF c_NoteColorHitImprecise = RGB(255, 205, 40);

// Colors the lock-in confetti is drawn from - a generic celebration, independent of clip colors.
// Editable separately from c_StreakColor/c_HighlightWhite even though two entries start matching.
inline COLORREF c_ConfettiPalette[] = {
    RGB(255, 70, 235), RGB(56, 219, 255), RGB(255, 205, 70), RGB(190, 140, 255), RGB(255, 205, 70), RGB(255, 255, 255),
};
constexpr int c_ConfettiPaletteSize = sizeof(c_ConfettiPalette) / sizeof(c_ConfettiPalette[0]);

// Gold for the bank readout, so it reads as its own kind of value, not one more green flash.
inline COLORREF c_ScorePopupBankedColor = RGB(255, 215, 90);

// --- MainWindow chrome (song list / toolbar) ---

// Matches NoteLane's palette so the whole window reads as one theme.
inline COLORREF c_WindowBgColor = RGB(15, 11, 30);
inline COLORREF c_FieldBgColor = RGB(30, 23, 56);
inline COLORREF c_LabelTextColor = RGB(230, 222, 245);
inline COLORREF c_RefreshButtonColor = RGB(255, 205, 70);
inline COLORREF c_AssignButtonColor = RGB(150, 220, 140);
inline COLORREF c_SongRowHighlightColor = RGB(56, 219, 255);
inline COLORREF c_SongRowHighlightTextColor = RGB(10, 10, 20);
inline COLORREF c_HintTextColor = RGB(150, 140, 175);
// Editable separately from c_FieldBgColor/c_SongRowHighlightColor even though they start matching.
inline COLORREF c_ToggleTrackOffColor = RGB(30, 23, 56);
inline COLORREF c_ToggleTrackOnColor = RGB(56, 219, 255);
inline COLORREF c_ToggleKnobColor = RGB(245, 242, 250);

} // namespace GameColors
