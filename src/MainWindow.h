#pragma once

#include <windows.h>

#include <array>

#include "AudioPlayer.h"
#include "BeatScroller.h"
#include "MusicTrack.h"
#include "Settings.h"

// Owns the app's single window: creates its controls, wires up playback and
// persistence, and hosts the beat-scroller animation. No global state -
// everything lives here as members.
class MainWindow
{
public:
    // Registers the window class and creates/shows the window.
    bool Create(HINSTANCE hInstance, int nCmdShow);

    // Runs the standard Win32 message loop until the window closes.
    int RunMessageLoop();

private:
    // Win32-mandated free-function thunk: recovers the MainWindow instance
    // via GWLP_USERDATA and forwards to HandleMessage.
    static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    // Dispatches a window message to the matching On* handler.
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    // Creates all child controls and the beat-scroller lane, then loads saved settings.
    void OnCreate(HWND hwnd);

    // Routes a WM_COMMAND control ID to the Browse or Play handler.
    void OnCommand(HWND hwnd, int controlId);

    // Saves settings, stops playback/animation, and quits the message loop.
    void OnDestroy();

    // Repaints the background and the beat-scroller.
    void OnPaint(HWND hwnd);

    // Starts or stops the beat-scroller animation in response to AudioPlayer's callback.
    void OnTrackStateChanged(int trackIndex, bool isPlaying);

    // Opens the audio file picker and stores the chosen path in the given row.
    void BrowseForAudioFile(HWND hwnd, int trackIndex);

    // Reads all tracks from the UI and starts playing the non-empty ones.
    void PlayAllTracks(HWND hwnd);

    // Loads saved tracks from Settings into the UI controls.
    void LoadTracksIntoUi();

    // Reads the current UI state and saves it via Settings.
    void SaveTracksFromUi();

    // Reads one row's controls into a MusicTrack, clamping repeat count and BPM to >= 1.
    MusicTrack ReadTrackFromUi(int trackIndex) const;

    HWND m_hwnd = nullptr;
    std::array<HWND, kTrackCount> m_hEditFilePath{};
    std::array<HWND, kTrackCount> m_hEditRepeatCount{};
    std::array<HWND, kTrackCount> m_hEditBpm{};

    AudioPlayer m_audioPlayer;
    Settings m_settings;
    BeatScroller m_beatScroller;
};
