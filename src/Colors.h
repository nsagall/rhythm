#pragma once

#include <windows.h>

// Single place for every hard-coded color used by Rhythm.exe (the game) -
// MainWindow's chrome and NoteLaneGdiRenderer's playfield/HUD both pull
// from here so the whole app reads as one palette and so an author tuning
// colors only ever has one file to open. RhythmEditor is a separate tool
// with its own ImGui theme and isn't covered by this file.
//
// Every value below is a mutable `inline` global, not a `constexpr` - the
// literal here is only the compiled-in default. ColorConfig.h's
// Colors::LoadFromIni() overwrites whichever of these Colors.ini actually
// has a saved value for at startup, and ColorEditor.exe (src/coloreditor/)
// edits + saves that same ini by writing straight through these globals'
// addresses (see ColorConfig.cpp's Colors::AllEntries()) - so this file,
// the ini format, and the editor tool can never disagree about what's
// editable or what a given name means.

// Which accent color a clip's own instance (see NoteLaneScene.h's
// ClipInstance) is identified by. A COLORREF is just a packed RGB value,
// not a drawing call - so which one a given clip gets is part of that
// clip's identity, not a decision any one renderer should own privately.
// Shared (not NoteLaneGdiRenderer-private) so every renderer gets the same
// answer for the same clip for free; a renderer that wants a different
// scheme entirely is still free to ignore ClipInstance::color and derive
// its own some other way.
namespace ClipColor
{

// Shown for a clip that isn't currently known/relevant to the game (Idle
// before a chart's loaded, or Complete) - a dim, desaturated neutral
// rather than any real clip's own color, so an empty lane reads as
// "nothing playing" rather than implying a specific clip that isn't
// actually there.
inline COLORREF c_Neutral = RGB(150, 150, 170);

// One accent color per clip, not per lane - a curated "candy" palette
// rather than a literal rainbow, so it reads as matched instead of
// clashing. Avoids red/green/yellow so a clip's own color is never
// mistaken for a hit/miss/imprecise-hit note flash (GameColors::
// c_NoteColorHit/c_NoteColorMiss/c_NoteColorHitImprecise) - kept entirely to
// the teal-through-pink side of the wheel rather than including any
// warm/yellow accent. Entry 0 is pushed toward magenta rather than a
// redder hue so it doesn't read as a near-miss of a miss-red. Cycles (see
// ForIndex) if a chart has more clips than colors here.
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

// Everything else: the playfield/HUD (NoteLaneGdiRenderer) and the
// surrounding window chrome (MainWindow) - both painted by hand with GDI,
// so both need their own explicit color constants rather than pulling from
// a system theme.
namespace GameColors
{

// --- Playfield background & general chrome (NoteLaneGdiRenderer) ---

// Deep indigo-to-violet backdrop - moody enough to make bright note colors
// pop, warm enough to not read as a cold "dev tool" dark mode.
inline COLORREF c_BgTop = RGB(46, 20, 92);
inline COLORREF c_BgBottom = RGB(15, 11, 42);
inline COLORREF c_BorderColor = RGB(120, 90, 190);
inline COLORREF c_TextColor = RGB(255, 250, 240);
inline COLORREF c_StreakColor = RGB(255, 205, 70); // still used as one of the confetti burst's colors

// Translucent panel fill shared by the HUD panel, the hits meter panel, and
// the debug overlay panel - one dark backing so every panel in the
// playfield reads as the same kind of surface. The alpha this fill is
// drawn at is a translucency amount, not a color, so it stays a plain
// compile-time constant rather than living in Colors.ini alongside it.
inline COLORREF c_PanelBgColor = RGB(12, 8, 28);
constexpr BYTE c_PanelBgAlpha = 175;

// Plain black/white used for drop shadows and small specular highlights
// (note glyphs, HUD text, sparkles, measure lines) - never a clip/judgement
// color, so kept generic here rather than duplicated as inline literals.
inline COLORREF c_ShadowColor = RGB(0, 0, 0);
inline COLORREF c_HighlightWhite = RGB(255, 255, 255);

// Pass/fail feedback: saturated, high-contrast green/red, pushed toward
// each color's most vivid extreme (rather than a softer pastel) so the
// judgement reads instantly at a glance - paired with a glow halo so it
// pops rather than just being a flat color swap. c_NoteColorHit doubles as
// the "correctly held, not yet released" color.
inline COLORREF c_NoteColorHit = RGB(40, 235, 80);
inline COLORREF c_NoteColorMiss = RGB(255, 30, 30);

// A hit whose press wasn't precise (see GameSession::JudgementEvent::precise/
// SceneNote::precise) - still a correct press, just a sloppy one - renders
// yellow instead of green: the ripple it spawns (OnJudgement), and (once
// SceneNote::state settles to Hit) the note block itself, both via
// ColorForNote. Receptor/flash colors stay c_NoteColorHit regardless - those
// are a single-frame press/release cue with no "how precise" of their own to
// show, unlike a note block, which stays on screen long enough for the
// distinction to actually be readable.
inline COLORREF c_NoteColorHitImprecise = RGB(255, 205, 40);

// Palette confetti pieces are drawn from at lock-in - a small fixed set of
// "party colors" of its own, deliberately independent of any clip's color
// (this is a generic celebration, not an identity cue). Independently
// editable from c_StreakColor/c_HighlightWhite even though two entries here
// happen to start out matching them - Colors.ini can drift them apart.
inline COLORREF c_ConfettiPalette[] = {
    RGB(255, 70, 235), RGB(56, 219, 255), RGB(255, 205, 70), RGB(190, 140, 255), RGB(255, 205, 70), RGB(255, 255, 255),
};
constexpr int c_ConfettiPaletteSize = sizeof(c_ConfettiPalette) / sizeof(c_ConfettiPalette[0]);

// Gold - used for the bank readout itself, so it reads as its own kind of
// value rather than one more green flash among the ripples/glyphs.
inline COLORREF c_ScorePopupBankedColor = RGB(255, 215, 90);

// --- MainWindow chrome (song list / toolbar) ---

// Matches NoteLane's palette so the whole window reads as one theme
// instead of a colorful game view floating in plain system-gray chrome.
inline COLORREF c_WindowBgColor = RGB(15, 11, 30);
inline COLORREF c_FieldBgColor = RGB(30, 23, 56);
inline COLORREF c_LabelTextColor = RGB(230, 222, 245);
inline COLORREF c_RefreshButtonColor = RGB(255, 205, 70);
inline COLORREF c_AssignButtonColor = RGB(150, 220, 140);
inline COLORREF c_SongRowHighlightColor = RGB(56, 219, 255);
inline COLORREF c_SongRowHighlightTextColor = RGB(10, 10, 20);
inline COLORREF c_HintTextColor = RGB(150, 140, 175);
// Independently editable from c_FieldBgColor/c_SongRowHighlightColor even
// though they start out matching (see c_ConfettiPalette's own comment).
inline COLORREF c_ToggleTrackOffColor = RGB(30, 23, 56);
inline COLORREF c_ToggleTrackOnColor = RGB(56, 219, 255);
inline COLORREF c_ToggleKnobColor = RGB(245, 242, 250);

} // namespace GameColors
