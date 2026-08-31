#include <cstdio>
#include <string>
#include <xinput.h>

#include "LaneBindings.h"
#include "MidiInputManager.h"
#include "Settings.h"

// Standalone diagnostic: verifies LaneBindings' matching/conflict-clearing/persistence and
// MidiInputManager::Unpack's byte extraction, headless - no window, audio, or MIDI device.
// Exercises the same code path the "Assign Inputs" capture flow drives.

namespace
{

int gFailures = 0;

void Check(bool condition, const char* description)
{
    printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition)
    {
        ++gFailures;
    }
}

void RunLaneBindingsTests()
{
    printf("=== LaneBindings ===\n");

    Settings settings;
    LaneBindings bindings;
    bindings.Load(settings); // no prior custom bindings expected in a fresh settings.ini

    // Defaults alone: J/K/L/; resolve to lanes 0..3, nothing else does.
    Check(bindings.LaneForKey('J') == 0, "default J resolves to lane 0");
    Check(bindings.LaneForKey('K') == 1, "default K resolves to lane 1");
    Check(bindings.LaneForKey('L') == 2, "default L resolves to lane 2");
    Check(bindings.LaneForKey(VK_OEM_1) == 3, "default ; resolves to lane 3");
    Check(bindings.LaneForKey('X') == -1, "unbound key resolves to no lane");
    Check(bindings.LaneForMidiNote(60) == -1, "no custom MIDI binding yet resolves to no lane");
    Check(bindings.LaneForGamepadButton(XINPUT_GAMEPAD_A) == -1, "no custom gamepad binding yet resolves to no lane");
    Check(bindings.IsDefaultKey('K'), "K is recognized as a default key");
    Check(!bindings.IsDefaultKey('X'), "X is not recognized as a default key");

    // Assign a custom keyboard key to lane 2 - jkl; must keep working alongside it.
    bindings.SetCustom(2, InputBinding{InputKind::Keyboard, 'X'}, settings);
    Check(bindings.LaneForKey('X') == 2, "custom key X resolves to lane 2");
    Check(bindings.LaneForKey('L') == 2, "default L for lane 2 still works alongside its custom binding");

    // Assign a MIDI note to lane 0.
    bindings.SetCustom(0, InputBinding{InputKind::MidiNote, 60}, settings);
    Check(bindings.LaneForMidiNote(60) == 0, "custom MIDI note 60 resolves to lane 0");
    Check(bindings.LaneForKey('J') == 0, "default J for lane 0 still works alongside its MIDI binding");

    // Conflict-clearing: assigning the same MIDI note to lane 1 must clear it from lane 0.
    bindings.SetCustom(1, InputBinding{InputKind::MidiNote, 60}, settings);
    Check(bindings.LaneForMidiNote(60) == 1, "reassigned MIDI note 60 now resolves to lane 1");
    Check(bindings.GetCustom(0).kind == InputKind::None, "lane 0's custom binding was cleared by the conflict");

    // Assign a gamepad button to lane 3 (currently unbound), alongside its default ; key.
    bindings.SetCustom(3, InputBinding{InputKind::Gamepad, XINPUT_GAMEPAD_A}, settings);
    Check(bindings.LaneForGamepadButton(XINPUT_GAMEPAD_A) == 3, "custom gamepad A button resolves to lane 3");
    Check(bindings.LaneForKey(VK_OEM_1) == 3, "default ; for lane 3 still works alongside its gamepad binding");

    // Conflict-clearing applies across gamepad bindings too: reassigning the
    // same button to lane 0 (also currently unbound) must clear it from lane 3.
    bindings.SetCustom(0, InputBinding{InputKind::Gamepad, XINPUT_GAMEPAD_A}, settings);
    Check(bindings.LaneForGamepadButton(XINPUT_GAMEPAD_A) == 0, "reassigned gamepad A button now resolves to lane 0");
    Check(bindings.GetCustom(3).kind == InputKind::None, "lane 3's custom binding was cleared by the conflict");

    // Replace semantics: reassigning lane 1's own custom binding drops the old one.
    bindings.SetCustom(1, InputBinding{InputKind::Keyboard, 'Y'}, settings);
    Check(bindings.LaneForKey('Y') == 1, "lane 1's new custom key Y resolves");
    Check(bindings.LaneForMidiNote(60) == -1, "lane 1's old MIDI note 60 no longer resolves anywhere");

    // Settings round-trip: a fresh LaneBindings loaded from the same Settings
    // instance should see everything just persisted.
    LaneBindings reloaded;
    reloaded.Load(settings);
    Check(reloaded.LaneForKey('X') == 2, "reload: lane 2's custom key X persisted");
    Check(reloaded.LaneForKey('Y') == 1, "reload: lane 1's custom key Y persisted");
    Check(reloaded.LaneForMidiNote(60) == -1, "reload: lane 0's cleared MIDI binding stayed cleared");
    Check(reloaded.LaneForKey('J') == 0, "reload: default J for lane 0 still works");
    Check(reloaded.LaneForGamepadButton(XINPUT_GAMEPAD_A) == 0, "reload: lane 0's custom gamepad A button persisted");

    // Clean up so re-running this diagnostic starts from a known state.
    settings.SaveLaneBinding(0, L"");
    settings.SaveLaneBinding(1, L"");
    settings.SaveLaneBinding(2, L"");
    settings.SaveLaneBinding(3, L"");
}

void RunMidiUnpackTests()
{
    printf("=== MidiInputManager::Unpack ===\n");

    // Note-on, channel 0, note 60 (middle C), velocity 100:
    // status=0x90, data1=60, data2=100 packed little-endian into the low 3 bytes.
    LPARAM noteOn = static_cast<LPARAM>(0x90 | (60 << 8) | (100 << 16));
    MidiInputManager::ShortMessage msgOn = MidiInputManager::Unpack(noteOn);
    Check(msgOn.status == 0x90, "note-on status byte unpacked");
    Check(msgOn.data1 == 60, "note-on note number unpacked");
    Check(msgOn.data2 == 100, "note-on velocity unpacked");

    // Note-off, channel 3, note 21, velocity 0: status=0x83.
    LPARAM noteOff = static_cast<LPARAM>(0x83 | (21 << 8) | (0 << 16));
    MidiInputManager::ShortMessage msgOff = MidiInputManager::Unpack(noteOff);
    Check(msgOff.status == 0x83, "note-off status byte unpacked");
    Check(msgOff.data1 == 21, "note-off note number unpacked");
    Check(msgOff.data2 == 0, "note-off velocity unpacked");
}

} // namespace

int main()
{
    RunLaneBindingsTests();
    RunMidiUnpackTests();

    printf("\n%s\n", gFailures == 0 ? "All checks passed." : "Some checks FAILED.");
    return gFailures == 0 ? 0 : 1;
}
