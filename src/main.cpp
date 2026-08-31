#include "MainWindow.h"

#include <mmsystem.h>

#include "ColorConfig.h"

// Entry point: creates the main window and runs the message loop until it closes.
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Overwrites Colors.h's compiled-in defaults with Colors.ini's saved values - must run before
    // MainWindow::Create, which reads them to create GDI brushes immediately.
    Colors::LoadFromIni();

    // Windows' default ~15.6ms timer resolution rounds/coalesces SetTimer's ~16ms frame timer, so
    // ticks slip or double up under load, delaying miss-timeout detection and audio-drift resync.
    // Raise it to 1ms for the process's lifetime; matched by timeEndPeriod on the way out.
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
