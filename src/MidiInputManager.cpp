#include "MidiInputManager.h"

MidiInputManager::~MidiInputManager()
{
    CloseAll();
}

void MidiInputManager::OpenAll(HWND hwnd)
{
    UINT deviceCount = midiInGetNumDevs();
    for (UINT deviceId = 0; deviceId < deviceCount; ++deviceId)
    {
        HMIDIIN device = nullptr;
        MMRESULT openResult = midiInOpen(&device, deviceId, reinterpret_cast<DWORD_PTR>(hwnd), 0, CALLBACK_WINDOW);
        if (openResult != MMSYSERR_NOERROR)
        {
            continue;
        }

        if (midiInStart(device) != MMSYSERR_NOERROR)
        {
            midiInClose(device);
            continue;
        }

        m_devices.push_back(device);
    }
}

void MidiInputManager::CloseAll()
{
    for (HMIDIIN device : m_devices)
    {
        midiInStop(device);
        midiInReset(device);
        midiInClose(device);
    }
    m_devices.clear();
}

MidiInputManager::ShortMessage MidiInputManager::Unpack(LPARAM lParam)
{
    DWORD packed = static_cast<DWORD>(lParam);
    ShortMessage message;
    message.status = static_cast<BYTE>(packed & 0xFF);
    message.data1 = static_cast<BYTE>((packed >> 8) & 0xFF);
    message.data2 = static_cast<BYTE>((packed >> 16) & 0xFF);
    return message;
}
