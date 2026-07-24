#pragma once

#include <windows.h>

#include <array>

#include "MusicTrack.h"

class TrackLights {
public:
    void Attach(HWND hwnd);
    void SetLightRect(int trackIndex, RECT rect);

    void Start(int trackIndex, int bpm);
    void Stop(int trackIndex);
    void StopAll();

    bool OnTimer(WPARAM timerId);
    void Draw(HDC hdc) const;

private:
    HWND m_hwnd = nullptr;
    std::array<RECT, kTrackCount> m_lightRects{};
    std::array<bool, kTrackCount> m_lightOn{};
};
