#include <windows.h>
#include <commdlg.h>

#include "AudioPlayer.h"

constexpr wchar_t kWindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rhythm";

constexpr int kMargin = 10;
constexpr int kControlHeight = 24;

enum ControlId : int {
    IDC_EDIT_FILEPATH = 101,
    IDC_BUTTON_BROWSE = 102,
    IDC_BUTTON_PLAY = 103,
    IDC_EDIT_REPEAT_COUNT = 104,
};

static HWND g_hEditFilePath = nullptr;
static HWND g_hButtonPlay = nullptr;
static HWND g_hEditRepeatCount = nullptr;
static AudioPlayer g_audioPlayer;

static void BrowseForWavFile(HWND hwnd) {
    wchar_t szFile[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"WAV Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_hEditFilePath, szFile);
        EnableWindow(g_hButtonPlay, TRUE);
    }
}

static void PlaySelectedFile(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    GetWindowTextW(g_hEditFilePath, path, MAX_PATH);

    if (wcslen(path) == 0) {
        MessageBoxW(hwnd, L"Please select a .wav file first.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t countText[16] = L"";
    GetWindowTextW(g_hEditRepeatCount, countText, 16);
    int repeatCount = _wtoi(countText);
    if (repeatCount < 1) {
        repeatCount = 1;
        SetWindowTextW(g_hEditRepeatCount, L"1");
    }

    g_audioPlayer.Play(path, repeatCount);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            HWND hLabel = CreateWindowExW(
                0, L"STATIC", L"WAV File:",
                WS_CHILD | WS_VISIBLE,
                kMargin, kMargin + 4, 70, kControlHeight,
                hwnd, nullptr, nullptr, nullptr
            );

            g_hEditFilePath = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                kMargin + 75, kMargin, 700, kControlHeight,
                hwnd, (HMENU)IDC_EDIT_FILEPATH, nullptr, nullptr
            );

            HWND hButtonBrowse = CreateWindowExW(
                0, L"BUTTON", L"Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                kMargin + 785, kMargin - 1, 100, kControlHeight + 2,
                hwnd, (HMENU)IDC_BUTTON_BROWSE, nullptr, nullptr
            );

            g_hButtonPlay = CreateWindowExW(
                0, L"BUTTON", L"Play",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                kMargin, kMargin + kControlHeight + 16, 100, 30,
                hwnd, (HMENU)IDC_BUTTON_PLAY, nullptr, nullptr
            );

            HWND hLabelRepeat = CreateWindowExW(
                0, L"STATIC", L"Play count:",
                WS_CHILD | WS_VISIBLE,
                kMargin + 120, kMargin + kControlHeight + 22, 70, kControlHeight,
                hwnd, nullptr, nullptr, nullptr
            );

            g_hEditRepeatCount = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"1",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                kMargin + 195, kMargin + kControlHeight + 18, 50, kControlHeight,
                hwnd, (HMENU)IDC_EDIT_REPEAT_COUNT, nullptr, nullptr
            );
            SendMessageW(g_hEditRepeatCount, EM_SETLIMITTEXT, 4, 0);

            SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hEditFilePath, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hButtonBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hButtonPlay, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hLabelRepeat, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hEditRepeatCount, WM_SETFONT, (WPARAM)hFont, TRUE);

            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_BUTTON_BROWSE:
                    BrowseForWavFile(hwnd);
                    return 0;
                case IDC_BUTTON_PLAY:
                    PlaySelectedFile(hwnd);
                    return 0;
            }
            break;
        }
        case WM_DESTROY:
            g_audioPlayer.Stop();
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassEx(&wc)) {
        return 0;
    }

    HWND hwnd = CreateWindowEx(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}
