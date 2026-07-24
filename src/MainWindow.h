#pragma once

#include <windows.h>

#include <array>

#include "AudioPlayer.h"
#include "BeatScroller.h"
#include "MusicTrack.h"
#include "Settings.h"

class MainWindow {
public:
    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

private:
    static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnCommand(HWND hwnd, int controlId);
    void OnDestroy();
    void OnPaint(HWND hwnd);
    void OnTrackStateChanged(int trackIndex, bool isPlaying);

    void BrowseForAudioFile(HWND hwnd, int trackIndex);
    void PlayAllTracks(HWND hwnd);
    void LoadTracksIntoUi();
    void SaveTracksFromUi();
    MusicTrack ReadTrackFromUi(int trackIndex) const;

    HWND m_hwnd = nullptr;
    std::array<HWND, kTrackCount> m_hEditFilePath{};
    std::array<HWND, kTrackCount> m_hEditRepeatCount{};
    std::array<HWND, kTrackCount> m_hEditBpm{};

    AudioPlayer m_audioPlayer;
    Settings m_settings;
    BeatScroller m_beatScroller;
};
