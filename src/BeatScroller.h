#pragma once

#include <windows.h>

#include <vector>

class BeatScroller {
public:
    void Attach(HWND hwnd);
    void SetLaneRect(RECT rect);

    void Start(int bpm);
    void Stop();

    bool OnTimer(WPARAM timerId);
    void Draw(HDC hdc) const;

private:
    void SpawnDot();
    int GetLineX() const;

    HWND m_hwnd = nullptr;
    RECT m_laneRect{};
    std::vector<DWORD> m_dotSpawnTimes;
    double m_speedPxPerMs = 0.0;
    DWORD m_dotLifetimeMs = 0;
    bool m_isPlaying = false;
};
