#pragma once

#include <windows.h>

#include "LaneConfig.h"  // c_LaneCount sizes c_LaneDefaultKeys and m_custom.

class Settings;

// Lane 0..3's hardcoded default keyboard bindings, left to right: j, k, l, ;. VK_OEM_1 is the
// US-layout ';'/':' key (Windows has no VK_SEMICOLON). Always live, on top of any custom binding.
constexpr int c_LaneDefaultKeys[c_LaneCount] = {'J', 'K', 'L', VK_OEM_1};

// What one input source is: a keyboard virtual-key code, a live MIDI note number (0-127), or a
// single XINPUT_GAMEPAD_* button bit. None is the "no custom binding" sentinel, never matched
// against a real event.
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

    // VK_* code for Keyboard, MIDI note number for MidiNote, XINPUT_GAMEPAD_* bit for Gamepad.
    int code = 0;
};

// Per-lane input bindings: each lane always responds to its c_LaneDefaultKeys entry (not stored
// here - see LaneForKey), plus at most one optional custom binding (keyboard key, MIDI note, or
// gamepad button) assigned via MainWindow's "Assign Inputs" flow. Custom bindings persist to
// Settings; the jkl; defaults are compile-time constants and need no persistence.
class LaneBindings
{
public:
    // Loads every lane's saved custom binding from settings. A lane with nothing saved keeps a
    // default-constructed (no custom) InputBinding.
    void Load(Settings& settings);

    // Returns the lane vkCode should press/release: whichever lane has it as its c_LaneDefaultKeys
    // entry OR as a custom Keyboard binding, or -1 if neither.
    int LaneForKey(int vkCode) const;

    // Returns the lane a live MIDI note should press/release, i.e. whichever lane has it as a
    // custom MidiNote binding, or -1. There is no MIDI default.
    int LaneForMidiNote(int note) const;

    // Returns the lane a gamepad button (a single XINPUT_GAMEPAD_* bit) should press/release, i.e.
    // whichever lane has it as a custom Gamepad binding, or -1. No default.
    int LaneForGamepadButton(int button) const;

    // True if vkCode is any lane's hardcoded default key. The capture flow uses this to reject
    // assigning a default key as another lane's custom binding, which would be ambiguous.
    bool IsDefaultKey(int vkCode) const;

    // Returns lane's own custom binding (InputKind::None if it has none).
    InputBinding GetCustom(int lane) const;

    // Sets lane's custom binding to binding and persists it. First clears this exact (kind, code)
    // from whichever other lane had it, so one physical input never maps to two lanes. Does not
    // check IsDefaultKey - the capture flow rejects a default key before calling this.
    void SetCustom(int lane, InputBinding binding, Settings& settings);

private:
    // Persists m_custom[lane] to settings under that lane's own key.
    void SaveLane(int lane, Settings& settings) const;

    InputBinding m_custom[c_LaneCount];
};
