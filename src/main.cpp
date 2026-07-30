#include "MainWindow.h"

#include <mmsystem.h>

// Entry point: creates the main window and runs the message loop until it closes.
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Windows' default timer resolution (~15.6ms) is what SetTimer's ~16ms
    // frame timer (NoteLane's redraw tick, which also drives GameSession::
    // Update()) actually gets rounded/coalesced against - at that
    // resolution, ticks can slip or double up under any system load,
    // delaying miss-timeout detection and the audio-drift resync in
    // GameSession::Update() by more than the nominal frame interval. Raising
    // it to 1ms for this process's lifetime makes that timer - and message
    // dispatch generally - noticeably more regular. Matched by timeEndPeriod
    // on the way out, since this setting is process-wide, not per-timer.
    timeBeginPeriod(1);

    MainWindow window;
    int result = 0;
    if (window.Create(hInstance, nCmdShow))
    {
        result = window.RunMessageLoop();
    }

    timeEndPeriod(1);
    return result;
}
