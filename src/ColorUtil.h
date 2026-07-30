#pragma once

#include <windows.h>

// Small per-channel color math shared by NoteLane and MainWindow's custom
// painting - GDI has no built-in color-blending helpers of its own.
namespace ColorUtil
{

// Clamps a single color channel into [0, 255].
inline int ClampChannel(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

// Returns color shifted toward white by amt (per channel).
inline COLORREF Lighten(COLORREF color, int amt)
{
    return RGB(ClampChannel(GetRValue(color) + amt), ClampChannel(GetGValue(color) + amt),
               ClampChannel(GetBValue(color) + amt));
}

// Returns color shifted toward black by amt (per channel).
inline COLORREF Darken(COLORREF color, int amt)
{
    return RGB(ClampChannel(GetRValue(color) - amt), ClampChannel(GetGValue(color) - amt),
               ClampChannel(GetBValue(color) - amt));
}

// Blends linearly from `from` to `to` as t goes 0 -> 1.
inline COLORREF LerpColor(COLORREF from, COLORREF to, double t)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    int r = static_cast<int>(GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t);
    int g = static_cast<int>(GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t);
    int b = static_cast<int>(GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t);
    return RGB(ClampChannel(r), ClampChannel(g), ClampChannel(b));
}

} // namespace ColorUtil
