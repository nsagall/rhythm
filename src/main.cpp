#include "MainWindow.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    MainWindow window;
    if (!window.Create(hInstance, nCmdShow)) {
        return 0;
    }
    return window.RunMessageLoop();
}
