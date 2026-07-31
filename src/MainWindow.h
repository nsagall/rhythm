#pragma once

#include <windows.h>

#include <vector>

#include "AudioEngine.h"
#include "GameSession.h"
#include "NoteLane.h"
#include "Settings.h"
#include "SongLibrary.h"

// Which of the app's two screens is currently showing - they share the same
// window/back buffer but nothing else, so exactly one of them is ever drawn
// or fed keyboard/mouse input at a time.
enum class UiScreen
{
    SongSelect, // a list of every song found under Content\, chosen by click or keyboard
    Playing,    // the note lane, exactly as before
};

// Owns the app's single window: on startup (and whenever asked) scrapes the
// Content folder for playable songs and shows them as a list; choosing one
// (by click, or by arrow-key highlight + any other key) loads and starts
// it, switching to the note lane. Once that song finishes, playback keeps
// going (whatever locked-in clips are still looping) but the window goes
// back to the song list. No global state - everything lives here as members.
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

    // Creates the refresh button and the note lane, then does the initial song scan.
    void OnCreate(HWND hwnd);

    // Repositions the refresh button and the note lane rect to fit the
    // current client area, and recomputes the song list's rect - shared by
    // OnCreate (initial layout) and OnSize (every resize), so there's one
    // source of truth for where everything goes instead of two copies of
    // the same arithmetic.
    void Layout();

    // Re-runs Layout() for the new client size.
    void OnSize();

    // Keeps the window from being resized smaller than the note lane can fit in.
    void OnGetMinMaxInfo(LPARAM lParam);

    // Routes a WM_COMMAND control ID to the refresh handler.
    void OnCommand(HWND hwnd, int controlId);

    // On the song list: Up/Down moves the highlight, any other key (besides
    // a pure modifier or Left/Right) chooses the highlighted song. While
    // playing: registers a press on a non-repeated key-down for one of the
    // lane keys (j/k/l/;), exactly as before.
    void OnKeyDown(WPARAM key, LPARAM flags);

    // Registers a release on key-up for one of the lane keys (j/k/l/;). Only meaningful while playing.
    void OnKeyUp(WPARAM key, LPARAM flags);

    // On the song list, clicking a row chooses that song immediately.
    void OnLButtonDown(LPARAM lParam);

    // Sends a lane press to the game session and reflects any judgement in the note lane.
    void RegisterPress(int lane);

    // Sends a lane release to the game session and reflects any judgement in the note lane.
    void RegisterRelease(int lane);

    // Advances the game session, reflects any judgement in the note lane,
    // and - once a song completes while it's the visible screen - switches
    // back to the song list (playback itself is left running untouched).
    void OnTimer(WPARAM timerId);

    // Stops the session/animation/audio engine and quits the message loop.
    void OnDestroy();

    // Repaints the background and whichever screen is current.
    void OnPaint(HWND hwnd);

    // Says "yes, I painted it" without actually erasing - the whole client
    // area is repainted every frame via the back buffer in OnPaint anyway,
    // so letting the default handler also erase it first would just be an
    // extra, visible flash before that repaint lands.
    void OnEraseBkgnd();

    // (Re)creates the off-screen back buffer to match the given size, if it
    // doesn't already match - everything is drawn into this buffer and then
    // blitted to the screen in one copy, instead of each individual GDI
    // call landing on screen separately, which is what was causing flicker.
    void EnsureBackBuffer(HDC referenceHdc, int width, int height);

    // Custom-paints the refresh button (WS_style BS_OWNERDRAW) as a
    // rounded, colored, bevelled button matching NoteLane's visual language
    // instead of a stock Win32 button.
    void OnDrawItem(LPARAM lParam);

    // Re-scrapes Content\ for songs, keeping the currently-highlighted
    // song's chart selected if it's still found (or falling back to the
    // last-played chart from Settings, or index 0), then repaints. Charts
    // that fail validation are always left out of the list either way -
    // reportValidationErrors (default false, for the initial scan at
    // startup) only controls whether a dialog then reports every one of
    // them; the explicit refresh button passes true.
    void RescanSongs(bool reportValidationErrors = false);

    // Loads and starts the given song and switches to the Playing screen -
    // shows an error dialog instead if the chart fails to load.
    void ChooseSong(int index);

    // Bails out of the current song (Esc while Playing) and returns to the
    // song list, stopping the session outright rather than leaving a
    // finished clip's loop running. No-op outside the Playing screen.
    void QuitToSongSelect();

    // Paints the song list: a title row per scraped song, the
    // currently-highlighted one drawn picked out from the rest.
    void DrawSongList(HDC hdc);

    // Returns the song list row index under the given client-space point, or -1 if none.
    int HitTestSongList(POINT pt) const;

    // Custom-paints the Easy Mode toggle switch on the song-select header
    // row - a rounded track (colored by state) with a circular knob, no
    // native control, matching how the song list itself is hand-drawn
    // rather than built from stock Win32 controls.
    void DrawEasyModeToggle(HDC hdc);

    HWND m_hwnd = nullptr;
    HWND m_hButtonRefresh = nullptr;
    HBRUSH m_windowBrush = nullptr;
    HBRUSH m_fieldBrush = nullptr;

    HDC m_backBufferDC = nullptr;
    HBITMAP m_backBufferBitmap = nullptr;
    int m_backBufferWidth = 0;
    int m_backBufferHeight = 0;

    UiScreen m_screen = UiScreen::SongSelect;
    std::vector<SongEntry> m_songs;
    int m_selectedSongIndex = -1;
    RECT m_songListRect{};

    // Easy Mode toggle: loaded from Settings on startup, flipped by
    // clicking m_easyModeToggleRect (SongSelect screen only), saved back to
    // Settings on every change, and passed to GameSession::LoadChart
    // whenever a song is chosen.
    bool m_easyMode = false;
    RECT m_easyModeToggleRect{};

    AudioEngine m_audioEngine;
    GameSession m_gameSession;
    NoteLane m_noteLane;
    Settings m_settings;
};
