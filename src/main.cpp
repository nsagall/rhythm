#include "MainWindow.h"

// Entry point: creates the main window and runs the message loop until it closes.
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    MainWindow window;
    if (!window.Create(hInstance, nCmdShow))
    {
        return 0;
    }
    return window.RunMessageLoop();
}
