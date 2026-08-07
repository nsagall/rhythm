#pragma once

#include <windows.h>
#include <xinput.h>

#include <array>
#include <vector>

// Thin wrapper over XInput: unlike keyboard/MIDI, Windows delivers no
// per-event message for a gamepad button, so this is polled once per frame
// (MainWindow::OnTimer, the same 16ms tick NoteLane already animates on)
// instead of reacting to a window message - see Poll(). Checks every
// XUSER_MAX_COUNT (4) controller slot each call, so - matching
// MidiInputManager's "any input device" story - there's no device picker UI;
// whichever slot a controller happens to be connected in just works.
class GamepadInputManager
{
public:
    // One button transition since the previous Poll() call: which single
    // XINPUT_GAMEPAD_* bit changed, and whether it went down (true, a press)
    // or up (false, a release).
    struct ButtonEvent
    {
        WORD button = 0;
        bool pressed = false;
    };

    // Reads every controller slot's current XINPUT_STATE and diffs it
    // against what that slot reported last call, returning one ButtonEvent
    // per bit that changed - call exactly once per frame. A slot with no
    // controller connected reads as all-buttons-up, so unplugging a
    // controller mid-press still reports the release rather than leaving a
    // lane stuck down.
    std::vector<ButtonEvent> Poll();

private:
    std::array<WORD, XUSER_MAX_COUNT> m_lastButtons{};
};
