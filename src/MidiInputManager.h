#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <vector>

// Thin wrapper over winmm's midiIn* API: opens every connected MIDI input device with
// CALLBACK_WINDOW, so note events arrive as MM_MIM_DATA messages on the target window's own message
// queue (handled on the same thread as WM_KEYDOWN), not a driver callback thread. No device picker
// - every connected device is opened at once; a note-number match resolves which lane (if any) it
// means (see LaneBindings::LaneForMidiNote).
class MidiInputManager
{
public:
    // Defensively closes anything still open.
    ~MidiInputManager();

    // Opens and starts every connected MIDI input device, targeting hwnd for its MM_MIM_DATA
    // messages. A device that fails to open is silently skipped - zero MIDI devices is normal.
    void OpenAll(HWND hwnd);

    // Stops and closes every device opened by OpenAll. Idempotent.
    void CloseAll();

    // One unpacked MIDI channel-voice short message: status byte (e.g. 0x90|ch note-on, 0x80|ch
    // note-off), then data1 = note number and data2 = velocity for a note on/off.
    struct ShortMessage
    {
        BYTE status = 0;
        BYTE data1 = 0;
        BYTE data2 = 0;
    };

    // Unpacks an MM_MIM_DATA message's lParam - the 3 MIDI bytes are packed little-endian into the
    // low 3 bytes. Narrows to the low 32 bits first, since LPARAM is 64-bit here.
    static ShortMessage Unpack(LPARAM lParam);

private:
    std::vector<HMIDIIN> m_devices;
};
