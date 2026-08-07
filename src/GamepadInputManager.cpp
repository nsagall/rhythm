#include "GamepadInputManager.h"

// See the header's own comment.
std::vector<GamepadInputManager::ButtonEvent> GamepadInputManager::Poll()
{
    std::vector<ButtonEvent> events;

    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot)
    {
        XINPUT_STATE state{};
        WORD buttons = 0;
        if (XInputGetState(slot, &state) == ERROR_SUCCESS)
        {
            buttons = state.Gamepad.wButtons;
        }

        WORD changed = buttons ^ m_lastButtons[slot];
        for (WORD bit = 1; bit != 0; bit = static_cast<WORD>(bit << 1))
        {
            if (changed & bit)
            {
                events.push_back(ButtonEvent{bit, (buttons & bit) != 0});
            }
        }
        m_lastButtons[slot] = buttons;
    }

    return events;
}
