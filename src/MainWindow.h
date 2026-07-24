#pragma once

#include <windows.h>

#include "AudioEngine.h"
#include "GameSession.h"
#include "NoteLane.h"
#include "Settings.h"

// Owns the app's single window: creates its controls, loads charts, routes
// keyboard taps to the GameSession, and hosts the NoteLane animation. No
// global state - everything lives here as members.
class MainWindow
{
public:
    MainWindow();

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

    // Creates all child controls and the note lane, then loads the last-used chart.
    void OnCreate(HWND hwnd);

    // Routes a WM_COMMAND control ID to the Browse or Start handler.
    void OnCommand(HWND hwnd, int controlId);

    // Registers a tap on a non-repeated spacebar key-down.
    void OnKeyDown(WPARAM key, LPARAM flags);

    // Advances the game session and reflects any judgement in the note lane.
    void OnTimer(WPARAM timerId);

    // Stops the session/animation/audio engine and quits the message loop.
    void OnDestroy();

    // Repaints the background and the note lane.
    void OnPaint(HWND hwnd);

    // Opens the chart file picker and loads the chosen chart.
    void BrowseForChart(HWND hwnd);

    HWND m_hwnd = nullptr;
    HWND m_hEditChartPath = nullptr;
    HWND m_hButtonStart = nullptr;

    AudioEngine m_audioEngine;
    GameSession m_gameSession;
    NoteLane m_noteLane;
    Settings m_settings;
};
