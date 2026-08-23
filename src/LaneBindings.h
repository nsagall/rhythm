#pragma once

#include <windows.h>

#include "LaneConfig.h"

class Settings;

// Lane 0..3's hardcoded default keyboard bindings, left to right: j, k, l, ;
// . VK_OEM_1 is the Win32 virtual-key constant for the US-layout ';'/':' key
// - there is no named VK_SEMICOLON constant in the Windows headers. Always
// live, on top of whatever LaneBindings::GetCustom adds - see LaneBindings'
// own comment for why a lane never loses these.
constexpr int c_LaneDefaultKeys[c_LaneCount] = {'J', 'K', 'L', VK_OEM_1};

// What one input source is: a keyboard virtual-key code, a live MIDI note
// number (0-127) from any connected MIDI input device, or a single
// XINPUT_GAMEPAD_* button bit from any connected Xbox controller. None is
// the "no custom binding set" sentinel - never itself matched against a real
// event.
enum class InputKind
{
    None,
    Keyboard,
    MidiNote,
    Gamepad,
};

struct InputBinding
{
    InputKind kind = InputKind::None;
    int code = 0; // VK_* code for Keyboard, MIDI note number for MidiNote, XINPUT_GAMEPAD_* bit for Gamepad
};

// Per-lane input bindings: each lane always responds to its own
// c_LaneDefaultKeys entry (unconditionally, not stored here - see
// LaneForKey), plus at most one optional custom binding assigned via the
// "Assign Inputs" flow (MainWindow's capture state machine) - a keyboard
// key, a live MIDI note, or a gamepad button, whichever one. Persisted to
// Settings so a custom binding survives a restart; the jkl; defaults need no
// persistence since they're compile-time constants.
class LaneBindings
{
public:
    // Loads every lane's saved custom binding from settings (a lane with
    // nothing saved keeps its default-constructed InputBinding, i.e. no
    // custom binding).
    void Load(Settings& settings);

    // Returns the lane vkCode should press/release right now: whichever
    // lane has vkCode as its c_LaneDefaultKeys entry, OR whichever lane has
    // it as a custom Keyboard binding - or -1 if neither.
    int LaneForKey(int vkCode) const;

    // Returns the lane a live MIDI note-on/note-off for `note` should
    // press/release, i.e. whichever lane has it as a custom MidiNote
    // binding - or -1 if none. Unlike LaneForKey, there is no MIDI default
    // to fall back to.
    int LaneForMidiNote(int note) const;

    // Returns the lane a gamepad button press/release for `button` (a single
    // XINPUT_GAMEPAD_* bit) should press/release, i.e. whichever lane has it
    // as a custom Gamepad binding - or -1 if none. Same "no default" story
    // as LaneForMidiNote.
    int LaneForGamepadButton(int button) const;

    // True if vkCode is any lane's hardcoded default key - used by the
    // capture flow to reject trying to assign a default key as some other
    // lane's custom binding (which would create an unresolvable ambiguity
    // between "is this the default lane's default, or the other lane's
    // custom binding" - simplest to just disallow it outright).
    bool IsDefaultKey(int vkCode) const;

    // Returns lane's own custom binding (InputKind::None if it doesn't have one).
    InputBinding GetCustom(int lane) const;

    // Sets lane's custom binding to binding and persists it via settings.
    // First clears this exact (kind, code) from whichever other lane (if
    // any) already had it as its own custom binding, and persists that
    // clear too - so a single physical input never ends up mapped to two
    // lanes at once. Does not check IsDefaultKey - the capture flow itself
    // is responsible for rejecting a default key before ever calling this.
    void SetCustom(int lane, InputBinding binding, Settings& settings);

private:
    // Persists m_custom[lane] to settings under that lane's own key.
    void SaveLane(int lane, Settings& settings) const;

    InputBinding m_custom[c_LaneCount];
};
