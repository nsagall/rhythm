#pragma once

#include <windows.h>

// Single place for every hard-coded color constant used by Rhythm.exe (the
// game) - MainWindow's chrome and NoteLaneGdiRenderer's playfield/HUD both
// pull from here so the whole app reads as one palette and so an author
// tuning colors only ever has one file to open. RhythmEditor is a separate
// tool with its own ImGui theme and isn't covered by this file.

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
constexpr COLORREF kNeutral = RGB(150, 150, 170);

// One accent color per clip, not per lane - a curated "candy" palette
// rather than a literal rainbow, so it reads as matched instead of
// clashing. Avoids red/green/yellow so a clip's own color is never
// mistaken for a hit/miss/imprecise-hit note flash (GameColors::
// kNoteColorHit/kNoteColorMiss/kNoteColorHitImprecise) - kept entirely to
// the teal-through-pink side of the wheel rather than including any
// warm/yellow accent. Entry 0 is pushed toward magenta rather than a
// redder hue so it doesn't read as a near-miss of a miss-red. Cycles (see
// ForIndex) if a chart has more clips than colors here.
constexpr COLORREF kPalette[] = {
    RGB(255, 70, 235),  // magenta
    RGB(56, 219, 255),  // electric cyan
    RGB(60, 225, 180),  // teal
    RGB(190, 140, 255), // violet
    RGB(70, 140, 255),  // azure
    RGB(255, 90, 170),  // hot pink
    RGB(215, 100, 255), // orchid
    RGB(130, 110, 255), // periwinkle
};
constexpr int kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// Returns the accent color for the clip at this position in
// ChartSong::clips, or kNeutral for index < 0 (no clip).
inline COLORREF ForIndex(int index)
{
    if (index < 0)
    {
        return kNeutral;
    }
    return kPalette[static_cast<size_t>(index) % kPaletteSize];
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
constexpr COLORREF kBgTop = RGB(46, 20, 92);
constexpr COLORREF kBgBottom = RGB(15, 11, 42);
constexpr COLORREF kBorderColor = RGB(120, 90, 190);
constexpr COLORREF kTextColor = RGB(255, 250, 240);
constexpr COLORREF kStreakColor = RGB(255, 205, 70); // still used as one of the confetti burst's colors

// Translucent panel fill shared by the HUD panel, the hits meter panel, and
// the debug overlay panel - one dark backing so every panel in the
// playfield reads as the same kind of surface.
constexpr COLORREF kPanelBgColor = RGB(12, 8, 28);
constexpr BYTE kPanelBgAlpha = 175;

// Plain black/white used for drop shadows and small specular highlights
// (note glyphs, HUD text, sparkles, measure lines) - never a clip/judgement
// color, so kept generic here rather than duplicated as inline literals.
constexpr COLORREF kShadowColor = RGB(0, 0, 0);
constexpr COLORREF kHighlightWhite = RGB(255, 255, 255);

// Pass/fail feedback: saturated, high-contrast green/red, pushed toward
// each color's most vivid extreme (rather than a softer pastel) so the
// judgement reads instantly at a glance - paired with a glow halo so it
// pops rather than just being a flat color swap. kNoteColorHit doubles as
// the "correctly held, not yet released" color.
constexpr COLORREF kNoteColorHit = RGB(40, 235, 80);
constexpr COLORREF kNoteColorMiss = RGB(255, 30, 30);

// A hit whose press wasn't precise (see GameSession::JudgementEvent::precise/
// SceneNote::precise) - still a correct press, just a sloppy one - renders
// yellow instead of green: the ripple it spawns (OnJudgement), and (once
// SceneNote::state settles to Hit) the note block itself, both via
// ColorForNote. Receptor/flash colors stay kNoteColorHit regardless - those
// are a single-frame press/release cue with no "how precise" of their own to
// show, unlike a note block, which stays on screen long enough for the
// distinction to actually be readable.
constexpr COLORREF kNoteColorHitImprecise = RGB(255, 205, 40);

// Palette confetti pieces are drawn from at lock-in - a small fixed set of
// "party colors" of its own, deliberately independent of any clip's color
// (this is a generic celebration, not an identity cue).
constexpr COLORREF kConfettiPalette[] = {
    RGB(255, 70, 235), RGB(56, 219, 255), RGB(255, 205, 70), RGB(190, 140, 255), kStreakColor, RGB(255, 255, 255),
};
constexpr int kConfettiPaletteSize = sizeof(kConfettiPalette) / sizeof(kConfettiPalette[0]);

// Gold - used for the bank readout itself, so it reads as its own kind of
// value rather than one more green flash among the ripples/glyphs.
constexpr COLORREF kScorePopupBankedColor = RGB(255, 215, 90);

// --- MainWindow chrome (song list / toolbar) ---

// Matches NoteLane's palette so the whole window reads as one theme
// instead of a colorful game view floating in plain system-gray chrome.
constexpr COLORREF kWindowBgColor = RGB(15, 11, 30);
constexpr COLORREF kFieldBgColor = RGB(30, 23, 56);
constexpr COLORREF kLabelTextColor = RGB(230, 222, 245);
constexpr COLORREF kRefreshButtonColor = RGB(255, 205, 70);
constexpr COLORREF kAssignButtonColor = RGB(150, 220, 140);
constexpr COLORREF kSongRowHighlightColor = RGB(56, 219, 255);
constexpr COLORREF kSongRowHighlightTextColor = RGB(10, 10, 20);
constexpr COLORREF kHintTextColor = RGB(150, 140, 175);
constexpr COLORREF kToggleTrackOffColor = kFieldBgColor;
constexpr COLORREF kToggleTrackOnColor = kSongRowHighlightColor;
constexpr COLORREF kToggleKnobColor = RGB(245, 242, 250);

} // namespace GameColors
