#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <vector>

// Thin wrapper over winmm's midiIn* API: opens every currently-connected MIDI
// input device and starts it delivering note events as ordinary window
// messages (CALLBACK_WINDOW, not CALLBACK_FUNCTION) - so a note-on/note-off
// arrives as an MM_MIM_DATA message on the target window's own message
// queue, handled on the same thread as WM_KEYDOWN and everything else,
// instead of on a driver-owned callback thread that would need its own
// synchronization against the game's state. No device enumeration/picker UI
// - "any input device" means every device that happens to be connected gets
// opened and listened to at once; a note number match is what actually
// resolves which lane (if any) it means (see LaneBindings::LaneForMidiNote).
class MidiInputManager
{
public:
    // Defensively closes anything still open, in case a caller forgets.
    ~MidiInputManager();

    // Opens and starts every currently-connected MIDI input device,
    // targeting hwnd for its MM_MIM_DATA messages. A device that fails to
    // open (e.g. claimed by another app) is silently skipped rather than
    // failing the whole call - matches AudioEngine::Initialize's own
    // soft-failure style, since having zero MIDI devices is the normal case
    // and shouldn't block anything keyboard-only still works fine.
    void OpenAll(HWND hwnd);

    // Stops and closes every device opened by OpenAll. Idempotent - a no-op
    // if nothing is open.
    void CloseAll();

    // One unpacked MIDI channel-voice short message.
    struct ShortMessage
    {
        BYTE status = 0; // e.g. 0x90 | channel for note-on, 0x80 | channel for note-off
        BYTE data1 = 0;  // note number for note-on/note-off
        BYTE data2 = 0;  // velocity for note-on/note-off
    };

    // Unpacks an MM_MIM_DATA message's lParam - the 3 MIDI bytes are packed
    // little-endian into the low 3 bytes of a 32-bit value, exactly like the
    // dwParam1 the CALLBACK_FUNCTION form would have delivered. Narrows to
    // the low 32 bits first since LPARAM is 64-bit on this project's build.
    static ShortMessage Unpack(LPARAM lParam);

private:
    std::vector<HMIDIIN> m_devices;
};
