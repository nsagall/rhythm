#pragma once

#include <windows.h>

#include <vector>

#include "AudioEngine.h"
#include "GamepadInputManager.h"
#include "GameSession.h"
#include "LaneBindings.h"
#include "MidiInputManager.h"
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

    // Routes a WM_COMMAND control ID to the refresh handler, or to
    // BeginCapture() for the Assign Inputs button.
    void OnCommand(HWND hwnd, int controlId);

    // While capturing an input assignment (m_captureLane != -1), every key
    // is owned by HandleCaptureKeyDown instead - see its own comment.
    // Otherwise, on the song list: Up/Down moves the highlight, any other
    // key (besides a pure modifier or Left/Right) chooses the highlighted
    // song. While playing: Space toggles pause; whichever lane
    // m_laneBindings resolves the key to (its default key, or a custom
    // binding - see LaneBindings::LaneForKey) registers a press on a
    // non-repeated key-down, but does nothing at all while paused (see
    // TogglePause).
    void OnKeyDown(WPARAM key, LPARAM flags);

    // Registers a release on key-up for whichever lane m_laneBindings
    // resolves the key to. Only meaningful while playing; a no-op during
    // capture (key-up mid-capture carries no meaning - only the next
    // key-down/MIDI note-on commits a binding).
    void OnKeyUp(WPARAM key, LPARAM flags);

    // Routes one physical key-down to the input-assignment capture flow
    // instead of normal gameplay: Escape cancels the remaining (not yet
    // captured) lanes via CancelCapture(); a reserved key (Escape/Space/D,
    // or any lane's own hardcoded default - see LaneBindings::IsDefaultKey)
    // is rejected with a re-prompt instead of being bound, since none of
    // those could ever actually reach OnKeyDown's own lane-matching once
    // bound (Escape/Space/D are handled earlier, unconditionally, and a
    // default key is already every lane's own fallback); any other key
    // commits as this lane's new custom binding via
    // LaneBindings::SetCustom, then AdvanceCapture() moves on.
    void HandleCaptureKeyDown(int vkCode);

    // Routes one live MIDI note-on/note-off (unpacked from an MM_MIM_DATA
    // message - see HandleMessage) the same way OnKeyDown/OnKeyUp handle a
    // physical key: during capture, a note-on commits this lane's new
    // custom MIDI binding (note-off is ignored - only the down edge
    // matters for capture); otherwise, only while Playing, a note-on/
    // note-off resolved via LaneBindings::LaneForMidiNote registers a press/
    // release exactly like a keyboard lane would. A note-on below
    // kMinMidiPressVelocity (MainWindow.cpp) is dropped entirely first -
    // neither a press nor a release - to filter out a sensitive
    // controller's accidental light taps.
    void OnMidiData(WPARAM wParam, LPARAM lParam);

    // Routes one gamepad button transition (as polled once per frame by
    // OnTimer via m_gamepadInput.Poll() - there's no window message for
    // this, unlike keyboard/MIDI) the same way OnMidiData handles a MIDI
    // note-on/note-off: during capture, a press commits this lane's new
    // custom Gamepad binding (a release is ignored - only the down edge
    // matters for capture); otherwise, only while Playing, a press/release
    // resolved via LaneBindings::LaneForGamepadButton registers a press/
    // release exactly like a keyboard or MIDI lane would.
    void OnGamepadButton(WORD button, bool pressed);

    // On the song list, clicking a row chooses that song immediately. A
    // no-op entirely while capturing (m_captureLane != -1) - a click can't
    // supply an input binding, and letting one through to ChooseSong would
    // leave the capture prompt dangling while a song starts underneath it.
    void OnLButtonDown(LPARAM lParam);

    // Pauses the game the moment this window is deactivated (Alt-Tab,
    // clicking another window, ...) - WM_ACTIVATE with WA_INACTIVE, not
    // WM_KILLFOCUS, since the latter also fires when keyboard focus merely
    // moves to a child control (e.g. the refresh button) without the window
    // itself losing the user's attention. Only while Playing; a no-op
    // otherwise, and never un-pauses on its own - regaining focus doesn't
    // resume, only Space does (see TogglePause).
    void OnActivate(WPARAM wParam);

    // Toggles GameSession::Pause()/Resume() and repaints so the HUD's
    // "Paused" text (or its absence) shows immediately. Only while Playing;
    // a no-op on the song list.
    void TogglePause();

    // Starts the "Assign Inputs" capture flow from the Song Select screen:
    // sets m_captureLane to 0 (the first lane to prompt for) and disables
    // the Assign/Refresh buttons for the run's duration, so neither a
    // second capture nor a song-list refresh can interleave with one
    // already in progress.
    void BeginCapture();

    // Moves capture to the next lane, or - once every lane has been
    // prompted - ends the run: resets m_captureLane to -1 and re-enables
    // the Assign/Refresh buttons.
    void AdvanceCapture();

    // Cancels the remaining (not yet captured) lanes on Escape. Lanes
    // already committed this run stay committed - each one is saved to
    // m_laneBindings/Settings immediately as it's captured (see
    // HandleCaptureKeyDown/OnMidiData), not batched until the whole run
    // finishes - so canceling partway through keeps whatever was already
    // assigned.
    void CancelCapture();

    // Draws the "press an input for lane N" prompt (naming the lane by its
    // 1-based index, not its default key) plus any rejection message from a
    // reserved-key attempt, on the Song Select screen while
    // m_captureLane != -1. Drawn from OnPaint right after DrawEasyModeToggle, using the same
    // CreateFontW/DrawTextW idiom as DrawSongList's own hint line.
    void DrawCapturePrompt(HDC hdc);

    // Sends a lane press to the game session, then drains and reflects any
    // judgement it produced (see DrainJudgements) - or, if this lane has no
    // note to press right now at all, shows the same miss feedback directly,
    // since GameSession::OnPress itself would otherwise just silently
    // ignore the tap (there being no judgement for DrainJudgements to find).
    void RegisterPress(int lane);

    // Sends a lane release to the game session, then drains and reflects
    // any judgement it produced (see DrainJudgements).
    void RegisterRelease(int lane);

    // Forwards every judgement GameSession has recorded since the last call
    // (see GameSession::ConsumeJudgementEvents) to the note lane - the one
    // place a judgement's visual feedback happens, regardless of whether it
    // came from an explicit key press/release (RegisterPress/
    // RegisterRelease) or from one of Update()'s own timeout checks (a note
    // never pressed, or held past its release window) - both are equally
    // real misses and get exactly the same flash/ripple treatment.
    void DrainJudgements();

    // Polls m_gamepadInput and routes every button transition it reports to
    // OnGamepadButton, advances the game session, drains and reflects any
    // judgement it produced, and - once a song completes while it's the
    // visible screen - switches back to the song list (playback itself is
    // left running untouched).
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

    // Easy Mode toggle: loaded from Settings on startup, flipped by
    // clicking m_easyModeToggleRect (SongSelect screen only), saved back to
    // Settings on every change, and passed to GameSession::LoadChart
    // whenever a song is chosen.
    bool m_easyMode = false;
    RECT m_easyModeToggleRect{};

    // Which lane (0..kLaneCount-1) is currently awaiting an input for the
    // "Assign Inputs" flow, or -1 when not capturing at all - see
    // BeginCapture/AdvanceCapture/CancelCapture. Only ever non-(-1) on the
    // SongSelect screen.
    int m_captureLane = -1;

    // Set by HandleCaptureKeyDown when a reserved key is pressed during
    // capture, shown by DrawCapturePrompt, cleared the next time a lane's
    // prompt is (re)shown for a fresh attempt.
    std::wstring m_captureRejectionMessage;

    AudioEngine m_audioEngine;
    GameSession m_gameSession;
    NoteLane m_noteLane;
    Settings m_settings;

    // Custom per-lane input bindings (keyboard or live MIDI note), on top of
    // kLaneDefaultKeys - see LaneBindings.h. Loaded from m_settings in
    // OnCreate, mutated (and persisted) by HandleCaptureKeyDown/OnMidiData
    // via LaneBindings::SetCustom.
    LaneBindings m_laneBindings;

    // Opens every connected MIDI input device on startup and delivers their
    // note events as MM_MIM_DATA window messages - see MidiInputManager.h
    // and OnMidiData.
    MidiInputManager m_midiInput;

    // Polled once per frame in OnTimer for Xbox controller button
    // transitions, since - unlike keyboard/MIDI - there's no window message
    // for those. See GamepadInputManager.h and OnGamepadButton.
    GamepadInputManager m_gamepadInput;
};
