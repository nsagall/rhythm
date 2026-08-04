#pragma once

#include <windows.h>

// Which accent color a clip's own instance (see NoteLaneScene.h's
// ClipInstance) is identified by. A COLORREF is just a packed RGB value,
// not a drawing call - so which one a given clip gets is part of that
// clip's identity, not a decision any one renderer should own privately.
// Shared (not NoteLaneGdiRenderer-private) so every renderer gets the same
// answer for the same clip for free; a renderer that wants a different
// scheme entirely is still free to ignore ClipInstance::color and derive
// its own from ClipInstance::index instead.
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
// clashing. Avoids red/green so a clip's own color is never mistaken for
// a hit/miss pass/fail flash; entry 0 is pushed toward magenta rather
// than a redder hue so it doesn't read as a near-miss of a miss-red.
// Cycles (see ForIndex) if a chart has more clips than colors here.
constexpr COLORREF kPalette[] = {
    RGB(255, 70, 235),  // magenta
    RGB(56, 219, 255),  // electric cyan
    RGB(255, 205, 70),  // gold
    RGB(190, 140, 255), // violet
    RGB(70, 140, 255),  // azure
    RGB(255, 90, 170),  // hot pink
    RGB(255, 170, 40),  // amber
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
