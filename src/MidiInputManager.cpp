#include "MidiInputManager.h"

MidiInputManager::~MidiInputManager()
{
    CloseAll();
}

// Opens and starts every currently-connected MIDI input device, targeting
// hwnd for CALLBACK_WINDOW delivery. A device that fails to open is skipped,
// not fatal - see the header's own comment.
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

// Stops and closes every device opened by OpenAll.
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

// Unpacks an MM_MIM_DATA message's lParam into its 3 MIDI bytes.
MidiInputManager::ShortMessage MidiInputManager::Unpack(LPARAM lParam)
{
    DWORD packed = static_cast<DWORD>(lParam);
    ShortMessage message;
    message.status = static_cast<BYTE>(packed & 0xFF);
    message.data1 = static_cast<BYTE>((packed >> 8) & 0xFF);
    message.data2 = static_cast<BYTE>((packed >> 16) & 0xFF);
    return message;
}
