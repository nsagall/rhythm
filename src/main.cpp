#include <windows.h>
#include <commdlg.h>

#include <array>
#include <vector>

#include "AudioPlayer.h"
#include "Settings.h"
#include "WavTrack.h"

constexpr wchar_t kWindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rhythm";

constexpr int kMargin = 10;
constexpr int kControlHeight = 24;
constexpr int kRowHeight = 34;
constexpr int kGroupBoxTop = 10;
constexpr int kGroupBoxLeft = 10;
constexpr int kGroupBoxWidth = 960;
constexpr int kRowTopInGroupBox = 30;

constexpr int IDC_EDIT_FILEPATH_BASE = 101;
constexpr int IDC_BUTTON_BROWSE_BASE = 110;
constexpr int IDC_EDIT_REPEAT_COUNT_BASE = 120;
constexpr int IDC_BUTTON_PLAY_ALL = 130;

static std::array<HWND, kTrackCount> g_hEditFilePath{};
static std::array<HWND, kTrackCount> g_hEditRepeatCount{};
static AudioPlayer g_audioPlayer;
static Settings g_settings;

static void BrowseForWavFile(HWND hwnd, int trackIndex) {
    wchar_t szFile[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Audio Files (*.wav;*.mp3)\0*.wav;*.mp3\0WAV Files (*.wav)\0*.wav\0MP3 Files (*.mp3)\0*.mp3\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_hEditFilePath[trackIndex], szFile);
    }
}

static void PlayAllTracks(HWND hwnd) {
    std::vector<WavTrack> tracks;

    for (int i = 0; i < kTrackCount; ++i) {
        wchar_t path[MAX_PATH] = L"";
        GetWindowTextW(g_hEditFilePath[i], path, MAX_PATH);
        if (wcslen(path) == 0) {
            continue;
        }

        wchar_t countText[16] = L"";
        GetWindowTextW(g_hEditRepeatCount[i], countText, 16);
        int repeatCount = _wtoi(countText);
        if (repeatCount < 1) {
            repeatCount = 1;
            SetWindowTextW(g_hEditRepeatCount[i], L"1");
        }

        tracks.push_back(WavTrack{path, repeatCount});
    }

    if (tracks.empty()) {
        MessageBoxW(hwnd, L"Please select at least one audio file.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_audioPlayer.PlayAll(std::move(tracks));
}

static void LoadTracksIntoUi() {
    std::array<WavTrack, kTrackCount> tracks = g_settings.Load();
    for (int i = 0; i < kTrackCount; ++i) {
        SetWindowTextW(g_hEditFilePath[i], tracks[i].filePath.c_str());
        SetWindowTextW(g_hEditRepeatCount[i], std::to_wstring(tracks[i].repeatCount).c_str());
    }
}

static void SaveTracksFromUi() {
    std::array<WavTrack, kTrackCount> tracks;
    for (int i = 0; i < kTrackCount; ++i) {
        wchar_t path[MAX_PATH] = L"";
        GetWindowTextW(g_hEditFilePath[i], path, MAX_PATH);

        wchar_t countText[16] = L"";
        GetWindowTextW(g_hEditRepeatCount[i], countText, 16);
        int repeatCount = _wtoi(countText);

        tracks[i].filePath = path;
        tracks[i].repeatCount = repeatCount < 1 ? 1 : repeatCount;
    }
    g_settings.Save(tracks);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            int groupBoxHeight = kRowTopInGroupBox + kTrackCount * kRowHeight + 10;
            HWND hGroupBox = CreateWindowExW(
                0, L"BUTTON", L"Tracks",
                WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                kGroupBoxLeft, kGroupBoxTop, kGroupBoxWidth, groupBoxHeight,
                hwnd, nullptr, nullptr, nullptr
            );
            SendMessageW(hGroupBox, WM_SETFONT, (WPARAM)hFont, TRUE);

            for (int i = 0; i < kTrackCount; ++i) {
                int rowY = kGroupBoxTop + kRowTopInGroupBox + i * kRowHeight;
                wchar_t trackLabel[16];
                wsprintfW(trackLabel, L"Track %d:", i + 1);

                HWND hLabel = CreateWindowExW(
                    0, L"STATIC", trackLabel,
                    WS_CHILD | WS_VISIBLE,
                    kGroupBoxLeft + 15, rowY + 4, 60, kControlHeight,
                    hwnd, nullptr, nullptr, nullptr
                );

                g_hEditFilePath[i] = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                    kGroupBoxLeft + 80, rowY, 620, kControlHeight,
                    hwnd, (HMENU)(INT_PTR)(IDC_EDIT_FILEPATH_BASE + i), nullptr, nullptr
                );

                HWND hButtonBrowse = CreateWindowExW(
                    0, L"BUTTON", L"Browse...",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    kGroupBoxLeft + 710, rowY - 1, 90, kControlHeight + 2,
                    hwnd, (HMENU)(INT_PTR)(IDC_BUTTON_BROWSE_BASE + i), nullptr, nullptr
                );

                HWND hLabelRepeat = CreateWindowExW(
                    0, L"STATIC", L"Play count:",
                    WS_CHILD | WS_VISIBLE,
                    kGroupBoxLeft + 810, rowY + 4, 75, kControlHeight,
                    hwnd, nullptr, nullptr, nullptr
                );

                g_hEditRepeatCount[i] = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"1",
                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                    kGroupBoxLeft + 890, rowY, 50, kControlHeight,
                    hwnd, (HMENU)(INT_PTR)(IDC_EDIT_REPEAT_COUNT_BASE + i), nullptr, nullptr
                );
                SendMessageW(g_hEditRepeatCount[i], EM_SETLIMITTEXT, 4, 0);

                SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(g_hEditFilePath[i], WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(hButtonBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(hLabelRepeat, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(g_hEditRepeatCount[i], WM_SETFONT, (WPARAM)hFont, TRUE);
            }

            HWND hButtonPlayAll = CreateWindowExW(
                0, L"BUTTON", L"Play All",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                kGroupBoxLeft, kGroupBoxTop + groupBoxHeight + 15, 120, 32,
                hwnd, (HMENU)IDC_BUTTON_PLAY_ALL, nullptr, nullptr
            );
            SendMessageW(hButtonPlayAll, WM_SETFONT, (WPARAM)hFont, TRUE);

            LoadTracksIntoUi();

            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);

            if (id >= IDC_BUTTON_BROWSE_BASE && id < IDC_BUTTON_BROWSE_BASE + kTrackCount) {
                BrowseForWavFile(hwnd, id - IDC_BUTTON_BROWSE_BASE);
                return 0;
            }

            if (id == IDC_BUTTON_PLAY_ALL) {
                PlayAllTracks(hwnd);
                return 0;
            }

            break;
        }
        case WM_DESTROY:
            SaveTracksFromUi();
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
