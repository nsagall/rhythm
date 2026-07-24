#include "MainWindow.h"

#include <commdlg.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rhythm";

constexpr int kControlHeight = 24;
constexpr int kRowHeight = 34;
constexpr int kGroupBoxTop = 10;
constexpr int kGroupBoxLeft = 10;
constexpr int kGroupBoxWidth = 960;
constexpr int kRowTopInGroupBox = 30;
constexpr int kLaneTopMargin = 20;
constexpr int kLaneHeight = 80;
constexpr int kPlayButtonTopMargin = 20;

constexpr int IDC_EDIT_FILEPATH_BASE = 101;
constexpr int IDC_BUTTON_BROWSE_BASE = 110;
constexpr int IDC_EDIT_REPEAT_COUNT_BASE = 120;
constexpr int IDC_BUTTON_PLAY = 130;
constexpr int IDC_EDIT_BPM_BASE = 140;

constexpr UINT WM_APP_TRACK_STATE = WM_APP + 1;

} // namespace

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WindowProcStatic;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd) {
        return false;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WindowProcStatic(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wParam));
            return 0;
        case WM_TIMER:
            if (m_beatScroller.OnTimer(wParam)) {
                return 0;
            }
            break;
        case WM_APP_TRACK_STATE:
            OnTrackStateChanged(static_cast<int>(wParam), lParam != 0);
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::OnCreate(HWND hwnd) {
    m_hwnd = hwnd;
    m_beatScroller.Attach(hwnd);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    int groupBoxHeight = kRowTopInGroupBox + kTrackCount * kRowHeight + 10;
    HWND hGroupBox = CreateWindowExW(
        0, L"BUTTON", L"Track",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        kGroupBoxLeft, kGroupBoxTop, kGroupBoxWidth, groupBoxHeight,
        hwnd, nullptr, nullptr, nullptr
    );
    SendMessageW(hGroupBox, WM_SETFONT, (WPARAM)hFont, TRUE);

    for (int i = 0; i < kTrackCount; ++i) {
        int rowY = kGroupBoxTop + kRowTopInGroupBox + i * kRowHeight;

        HWND hLabel = CreateWindowExW(
            0, L"STATIC", L"Track:",
            WS_CHILD | WS_VISIBLE,
            kGroupBoxLeft + 15, rowY + 4, 60, kControlHeight,
            hwnd, nullptr, nullptr, nullptr
        );

        m_hEditFilePath[i] = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
            kGroupBoxLeft + 80, rowY, 500, kControlHeight,
            hwnd, (HMENU)(INT_PTR)(IDC_EDIT_FILEPATH_BASE + i), nullptr, nullptr
        );

        HWND hButtonBrowse = CreateWindowExW(
            0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            kGroupBoxLeft + 590, rowY - 1, 90, kControlHeight + 2,
            hwnd, (HMENU)(INT_PTR)(IDC_BUTTON_BROWSE_BASE + i), nullptr, nullptr
        );

        HWND hLabelRepeat = CreateWindowExW(
            0, L"STATIC", L"Play count:",
            WS_CHILD | WS_VISIBLE,
            kGroupBoxLeft + 690, rowY + 4, 70, kControlHeight,
            hwnd, nullptr, nullptr, nullptr
        );

        m_hEditRepeatCount[i] = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            kGroupBoxLeft + 765, rowY, 45, kControlHeight,
            hwnd, (HMENU)(INT_PTR)(IDC_EDIT_REPEAT_COUNT_BASE + i), nullptr, nullptr
        );
        SendMessageW(m_hEditRepeatCount[i], EM_SETLIMITTEXT, 4, 0);

        HWND hLabelBpm = CreateWindowExW(
            0, L"STATIC", L"BPM:",
            WS_CHILD | WS_VISIBLE,
            kGroupBoxLeft + 818, rowY + 4, 35, kControlHeight,
            hwnd, nullptr, nullptr, nullptr
        );

        m_hEditBpm[i] = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"120",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            kGroupBoxLeft + 858, rowY, 45, kControlHeight,
            hwnd, (HMENU)(INT_PTR)(IDC_EDIT_BPM_BASE + i), nullptr, nullptr
        );
        SendMessageW(m_hEditBpm[i], EM_SETLIMITTEXT, 3, 0);

        SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hEditFilePath[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hButtonBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hLabelRepeat, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hEditRepeatCount[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hLabelBpm, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hEditBpm[i], WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    RECT laneRect;
    laneRect.left = kGroupBoxLeft;
    laneRect.top = kGroupBoxTop + groupBoxHeight + kLaneTopMargin;
    laneRect.right = kGroupBoxLeft + kGroupBoxWidth;
    laneRect.bottom = laneRect.top + kLaneHeight;
    m_beatScroller.SetLaneRect(laneRect);

    HWND hButtonPlay = CreateWindowExW(
        0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        kGroupBoxLeft, laneRect.bottom + kPlayButtonTopMargin, 120, 32,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_PLAY, nullptr, nullptr
    );
    SendMessageW(hButtonPlay, WM_SETFONT, (WPARAM)hFont, TRUE);

    m_audioPlayer.SetTrackStateCallback([hwnd](int trackIndex, bool isPlaying) {
        PostMessageW(hwnd, WM_APP_TRACK_STATE, static_cast<WPARAM>(trackIndex), static_cast<LPARAM>(isPlaying));
    });

    LoadTracksIntoUi();
}

void MainWindow::OnCommand(HWND hwnd, int controlId) {
    if (controlId >= IDC_BUTTON_BROWSE_BASE && controlId < IDC_BUTTON_BROWSE_BASE + kTrackCount) {
        BrowseForAudioFile(hwnd, controlId - IDC_BUTTON_BROWSE_BASE);
        return;
    }

    if (controlId == IDC_BUTTON_PLAY) {
        PlayAllTracks(hwnd);
        return;
    }
}

void MainWindow::OnDestroy() {
    SaveTracksFromUi();
    m_audioPlayer.Stop();
    m_beatScroller.Stop();
    PostQuitMessage(0);
}

void MainWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
    m_beatScroller.Draw(hdc);
    EndPaint(hwnd, &ps);
}

void MainWindow::OnTrackStateChanged(int trackIndex, bool isPlaying) {
    if (isPlaying) {
        wchar_t bpmText[16] = L"";
        GetWindowTextW(m_hEditBpm[trackIndex], bpmText, 16);
        int bpm = _wtoi(bpmText);
        m_beatScroller.Start(bpm < 1 ? 1 : bpm);
    } else {
        m_beatScroller.Stop();
    }
}

void MainWindow::BrowseForAudioFile(HWND hwnd, int trackIndex) {
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
        SetWindowTextW(m_hEditFilePath[trackIndex], szFile);
    }
}

void MainWindow::PlayAllTracks(HWND hwnd) {
    std::vector<MusicTrack> tracks;
    tracks.reserve(kTrackCount);
    bool anySelected = false;

    for (int i = 0; i < kTrackCount; ++i) {
        MusicTrack track = ReadTrackFromUi(i);
        if (!track.filePath.empty()) {
            anySelected = true;
        }
        tracks.push_back(std::move(track));
    }

    if (!anySelected) {
        MessageBoxW(hwnd, L"Please select at least one audio file.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    m_audioPlayer.PlayAll(std::move(tracks));
}

void MainWindow::LoadTracksIntoUi() {
    std::array<MusicTrack, kTrackCount> tracks = m_settings.Load();
    for (int i = 0; i < kTrackCount; ++i) {
        SetWindowTextW(m_hEditFilePath[i], tracks[i].filePath.c_str());
        SetWindowTextW(m_hEditRepeatCount[i], std::to_wstring(tracks[i].repeatCount).c_str());
        SetWindowTextW(m_hEditBpm[i], std::to_wstring(tracks[i].bpm).c_str());
    }
}

void MainWindow::SaveTracksFromUi() {
    std::array<MusicTrack, kTrackCount> tracks;
    for (int i = 0; i < kTrackCount; ++i) {
        tracks[i] = ReadTrackFromUi(i);
    }
    m_settings.Save(tracks);
}

MusicTrack MainWindow::ReadTrackFromUi(int trackIndex) const {
    MusicTrack track;

    wchar_t path[MAX_PATH] = L"";
    GetWindowTextW(m_hEditFilePath[trackIndex], path, MAX_PATH);
    track.filePath = path;

    wchar_t countText[16] = L"";
    GetWindowTextW(m_hEditRepeatCount[trackIndex], countText, 16);
    int repeatCount = _wtoi(countText);
    track.repeatCount = repeatCount < 1 ? 1 : repeatCount;

    wchar_t bpmText[16] = L"";
    GetWindowTextW(m_hEditBpm[trackIndex], bpmText, 16);
    int bpm = _wtoi(bpmText);
    track.bpm = bpm < 1 ? 1 : bpm;

    return track;
}
