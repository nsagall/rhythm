#pragma once

#include <windows.h>

#include <vector>

// Every include below is for a by-value member of MainWindow (SongLibrary.h for SongEntry in a vector).
#include "AudioEngine.h"
#include "GamepadInputManager.h"
#include "GameSession.h"
#include "LaneBindings.h"
#include "MidiInputManager.h"
#include "NoteLane.h"
#include "Settings.h"
#include "SongLibrary.h"

// Which of the app's two screens is currently showing. They share the window/back buffer but
// nothing else; exactly one is drawn and fed input at a time.
enum class UiScreen
{
    SongSelect, // A list of every song found under Content\, chosen by click or keyboard.
    Playing,    // The note lane.
};

// Owns the app's single window: scrapes the Content folder for playable songs and shows them as a
// list; choosing one loads and starts it, switching to the note lane. When the song finishes,
// playback keeps going but the window returns to the song list. No global state.
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

    // Repositions the refresh button, note lane rect, and song list rect to fit the current client
    // area. Shared by OnCreate and OnSize.
    void Layout();

    // Re-runs Layout() for the new client size.
    void OnSize();

    // Keeps the window from being resized smaller than the note lane can fit in.
    void OnGetMinMaxInfo(LPARAM lParam);

    // Routes a WM_COMMAND control ID to the refresh handler, or to
    // BeginCapture() for the Assign Inputs button.
    void OnCommand(HWND hwnd, int controlId);

    // While capturing an input assignment, every key goes to HandleCaptureKeyDown. Otherwise, on
    // the song list: Up/Down moves the highlight, any other key chooses the highlighted song. While
    // playing: Space toggles pause; a lane m_laneBindings resolves the key to registers a press on
    // a non-repeated key-down (nothing while paused).
    void OnKeyDown(WPARAM key, LPARAM flags);

    // Registers a release for whichever lane m_laneBindings resolves the key to. Only meaningful
    // while playing; a no-op during capture.
    void OnKeyUp(WPARAM key, LPARAM flags);

    // Routes one key-down to the input-assignment capture flow: Escape cancels the remaining lanes;
    // a reserved key (Escape/Space/D, or any lane's default) is rejected with a re-prompt; any
    // other key commits as this lane's custom binding, then AdvanceCapture() moves on.
    void HandleCaptureKeyDown(int vkCode);

    // Routes one live MIDI note-on/note-off the way OnKeyDown/OnKeyUp handle a key: during capture
    // a note-on commits this lane's custom MIDI binding (note-off ignored); otherwise, while
    // Playing, a note resolved via LaneBindings::LaneForMidiNote registers a press/release. A
    // note-on below c_MinMidiPressVelocity is dropped entirely, to filter accidental light taps.
    void OnMidiData(WPARAM wParam, LPARAM lParam);

    // Routes one gamepad button transition (polled once per frame by OnTimer) the way OnMidiData
    // handles a MIDI note: during capture a press commits this lane's custom Gamepad binding
    // (release ignored); otherwise, while Playing, a press/release resolved via
    // LaneBindings::LaneForGamepadButton registers one.
    void OnGamepadButton(WORD button, bool pressed);

    // On the song list, clicking a row chooses that song. A no-op while capturing, since a click
    // can't supply a binding.
    void OnLButtonDown(LPARAM lParam);

    // Pauses the game the moment this window is deactivated - WM_ACTIVATE with WA_INACTIVE, not
    // WM_KILLFOCUS (which also fires when focus moves to a child control). Only while Playing;
    // never un-pauses on its own (only Space does).
    void OnActivate(WPARAM wParam);

    // Toggles GameSession::Pause()/Resume() and repaints so the HUD's "Paused" text updates
    // immediately. Only while Playing.
    void TogglePause();

    // Starts the "Assign Inputs" capture flow: sets m_captureLane to 0 and disables the
    // Assign/Refresh buttons for the run, so nothing can interleave with one in progress.
    void BeginCapture();

    // Moves capture to the next lane, or - once every lane is prompted - ends the run: resets
    // m_captureLane to -1 and re-enables the Assign/Refresh buttons.
    void AdvanceCapture();

    // Cancels the remaining lanes on Escape. Lanes committed this run stay committed - each is
    // saved as it's captured, not batched.
    void CancelCapture();

    // Draws the "press an input for lane N" prompt (naming the lane by 1-based index) plus any
    // reserved-key rejection message, on Song Select while m_captureLane != -1.
    void DrawCapturePrompt(HDC hdc);

    // Sends a lane press to the game session, then drains and reflects any judgement (see
    // DrainJudgements). If this lane has no note to press, shows the miss feedback directly, since
    // GameSession::OnPress would otherwise silently ignore the tap.
    void RegisterPress(int lane);

    // Sends a lane release to the game session, then drains and reflects any judgement.
    void RegisterRelease(int lane);

    // Forwards every judgement/HUD-change/SFX-cue event GameSession recorded since the last call:
    // judgements and HUD changes go to the note lane, SFX cues play through m_audioEngine. Events
    // from Update()'s own checks get the same treatment as explicit press/release ones.
    void DrainJudgements();

    // Polls m_gamepadInput and routes each transition to OnGamepadButton, advances the game
    // session, drains judgements, and - once a song completes while this is the visible screen -
    // switches back to the song list (playback left running).
    void OnTimer(WPARAM timerId);

    // Stops the session/animation/audio engine and quits the message loop.
    void OnDestroy();

    // Repaints the background and whichever screen is current.
    void OnPaint(HWND hwnd);

    // Says "yes, I painted it" without erasing - OnPaint repaints the whole client area via the
    // back buffer every frame, so a default erase would just flash first.
    void OnEraseBkgnd();

    // (Re)creates the off-screen back buffer to match the given size. Everything is drawn into it
    // and blitted to the screen in one copy, to avoid flicker.
    void EnsureBackBuffer(HDC referenceHdc, int width, int height);

    // Custom-paints the refresh button (BS_OWNERDRAW) as a rounded, bevelled button matching
    // NoteLane's visual language.
    void OnDrawItem(LPARAM lParam);

    // Re-scrapes Content\ for songs, keeping the highlighted song selected if still found (else the
    // last-played chart, else index 0), then repaints.
    //   reportValidationErrors - if true (the refresh button), a dialog reports every chart that
    //                            failed validation; if false (the startup scan), they're silently omitted.
    void RescanSongs(bool reportValidationErrors = false);

    // Loads and starts the song at index and switches to the Playing screen - shows an error
    // dialog instead if the chart fails to load.
    void ChooseSong(int index);

    // Bails out of the current song (Esc while Playing) and returns to the song list, stopping the
    // session outright. No-op outside the Playing screen.
    void QuitToSongSelect();

    // Called from OnTimer when a song naturally finishes. Sets m_lastResultText for the song list
    // and, if the score earns a high-score spot, records it immediately (under a fixed placeholder,
    // no initials prompt) and refreshes m_songBestScores. Not called for QuitToSongSelect.
    void HandleSongComplete();

    // Paints the song list: a title row per song (with its best score, if any), the highlighted
    // one picked out.
    void DrawSongList(HDC hdc);

    // Draws m_lastResultText in the gap between the header row and the song list.
    void DrawLastResult(HDC hdc);

    // Returns the song list row index under the given client-space point, or -1 if none.
    int HitTestSongList(POINT pt) const;

    // Custom-paints the Easy Mode toggle switch on the song-select header row - a rounded track
    // with a circular knob, hand-drawn like the song list rather than a native control.
    void DrawEasyModeToggle(HDC hdc);

    HWND m_hwnd = nullptr;
    HWND m_hButtonRefresh = nullptr;
    HWND m_hButtonAssign = nullptr;
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

    // Parallel to m_songs: each song's best score (its high-score list's #1), or -1 if none.
    // Rebuilt with m_songs (see RescanSongs), patched in place by HandleSongComplete. DrawSongList
    // reads this, never Settings directly.
    std::vector<int> m_songBestScores;

    // The chart path/title GameSession is playing (or last played) - captured by ChooseSong, since
    // m_selectedSongIndex could drift before HandleSongComplete reads it back.
    std::wstring m_playingChartPath;
    std::wstring m_playingSongTitle;

    // Set by HandleSongComplete, shown by DrawLastResult until the next song is chosen.
    std::wstring m_lastResultText;

    // Easy Mode toggle: loaded from Settings on startup, flipped by clicking m_easyModeToggleRect,
    // saved on every change, passed to GameSession::LoadChart when a song is chosen.
    bool m_easyMode = false;
    RECT m_easyModeToggleRect{};

    // Which lane is currently awaiting an input for the "Assign Inputs" flow, or -1 when not
    // capturing. Only ever non-(-1) on the SongSelect screen.
    int m_captureLane = -1;

    // Set by HandleCaptureKeyDown when a reserved key is pressed during capture, shown by
    // DrawCapturePrompt, cleared when a lane's prompt is next (re)shown.
    std::wstring m_captureRejectionMessage;

    AudioEngine m_audioEngine;

    // One-shot HUD cues, loaded once in OnCreate from Content/sfx/. Invalid if the .wav is missing
    // or unsupported - PlaySfx on an invalid handle is a harmless no-op, so a missing file just
    // means silence.
    SfxHandle m_sfxMultiplierUp;
    SfxHandle m_sfxStreakBroken;

    GameSession m_gameSession;
    NoteLane m_noteLane;
    Settings m_settings;

    // Custom per-lane input bindings on top of c_LaneDefaultKeys. Loaded from m_settings in
    // OnCreate, mutated and persisted by HandleCaptureKeyDown/OnMidiData.
    LaneBindings m_laneBindings;

    // Opens every connected MIDI input device on startup, delivering note events as MM_MIM_DATA
    // window messages (see OnMidiData).
    MidiInputManager m_midiInput;

    // Polled once per frame in OnTimer for Xbox controller button transitions (no window message
    // for those). See OnGamepadButton.
    GamepadInputManager m_gamepadInput;
};
