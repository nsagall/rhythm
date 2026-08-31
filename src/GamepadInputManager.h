#pragma once

#include <windows.h>
#include <xinput.h>

#include <array>
#include <vector>

// Thin wrapper over XInput. Windows delivers no per-event message for a gamepad button, so this is
// polled once per frame (MainWindow::OnTimer's 16ms tick) - see Poll(). Checks every
// XUSER_MAX_COUNT slot each call; no device picker, whichever slot a controller is in just works.
class GamepadInputManager
{
public:
    // One button transition since the previous Poll(): which XINPUT_GAMEPAD_* bit changed, and
    // whether it went down (true) or up (false).
    struct ButtonEvent
    {
        WORD button = 0;
        bool pressed = false;
    };

    // Reads every controller slot's XINPUT_STATE and diffs it against last call, returning one
    // ButtonEvent per changed bit. Call once per frame. A slot with no controller reads as
    // all-buttons-up, so unplugging mid-press still reports the release.
    std::vector<ButtonEvent> Poll();

private:
    std::array<WORD, XUSER_MAX_COUNT> m_lastButtons{};
};
