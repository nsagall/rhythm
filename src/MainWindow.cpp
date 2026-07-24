#include "MainWindow.h"

#include <commdlg.h>

namespace
{

constexpr wchar_t kWindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rhythm";

constexpr int kControlHeight = 24;
constexpr int kRowTop = 10;
constexpr int kRowLeft = 10;
constexpr int kLaneTop = 50;
constexpr int kLaneLeft = 10;
constexpr int kLaneRight = 1010;
constexpr int kLaneBottom = 450;

constexpr int IDC_EDIT_CHARTPATH = 101;
constexpr int IDC_BUTTON_BROWSE = 110;
constexpr int IDC_BUTTON_START = 120;

} // namespace

// Binds the game session to this window's audio engine.
MainWindow::MainWindow() : m_gameSession(m_audioEngine)
{
}

// Registers the window class and creates/shows the window.
bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow)
{
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

    if (!RegisterClassExW(&wc))
    {
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

    if (!hwnd)
    {
        return false;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return true;
}

// Runs the standard Win32 message loop until the window closes.
int MainWindow::RunMessageLoop()
{
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// Win32-mandated free-function thunk: recovers the MainWindow instance via GWLP_USERDATA and forwards to HandleMessage.
LRESULT CALLBACK MainWindow::WindowProcStatic(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
    {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Dispatches a window message to the matching On* handler.
LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wParam));
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(wParam, lParam);
            return 0;
        case WM_TIMER:
            OnTimer(wParam);
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

// Creates all child controls and the note lane, then loads the last-used chart.
void MainWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    HWND hLabel = CreateWindowExW(
        0, L"STATIC", L"Chart:",
        WS_CHILD | WS_VISIBLE,
        kRowLeft, kRowTop + 4, 50, kControlHeight,
        hwnd, nullptr, nullptr, nullptr
    );

    m_hEditChartPath = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
        kRowLeft + 55, kRowTop, 650, kControlHeight,
        hwnd, (HMENU)(INT_PTR)IDC_EDIT_CHARTPATH, nullptr, nullptr
    );

    HWND hButtonBrowse = CreateWindowExW(
        0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        kRowLeft + 715, kRowTop - 1, 90, kControlHeight + 2,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_BROWSE, nullptr, nullptr
    );

    m_hButtonStart = CreateWindowExW(
        0, L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        kRowLeft + 815, kRowTop - 1, 100, kControlHeight + 2,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_START, nullptr, nullptr
    );

    SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(m_hEditChartPath, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hButtonBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(m_hButtonStart, WM_SETFONT, (WPARAM)hFont, TRUE);

    RECT laneRect{kLaneLeft, kLaneTop, kLaneRight, kLaneBottom};
    m_noteLane.Attach(hwnd);
    m_noteLane.SetLaneRect(laneRect);
    m_noteLane.StartAnimating();

    if (!m_audioEngine.Initialize())
    {
        MessageBoxW(hwnd, L"Failed to initialize the audio engine.", kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    std::wstring lastChartPath = m_settings.LoadLastChartPath();
    if (!lastChartPath.empty() && m_gameSession.LoadChart(lastChartPath))
    {
        SetWindowTextW(m_hEditChartPath, lastChartPath.c_str());
    }
}

// Routes a WM_COMMAND control ID to the Browse or Start handler.
void MainWindow::OnCommand(HWND hwnd, int controlId)
{
    if (controlId == IDC_BUTTON_BROWSE)
    {
        BrowseForChart(hwnd);
        return;
    }

    if (controlId == IDC_BUTTON_START)
    {
        m_gameSession.Start();
        return;
    }
}

// Registers a tap on a non-repeated spacebar key-down.
void MainWindow::OnKeyDown(WPARAM key, LPARAM flags)
{
    bool isAutoRepeat = (flags & (1 << 30)) != 0;
    if (key != VK_SPACE || isAutoRepeat)
    {
        return;
    }

    m_gameSession.OnTap();
    JudgementResult result = m_gameSession.ConsumeLastJudgement();
    if (result != JudgementResult::None)
    {
        m_noteLane.ShowJudgement(result);
    }
}

// Advances the game session and reflects any judgement in the note lane.
void MainWindow::OnTimer(WPARAM timerId)
{
    m_gameSession.Update();

    JudgementResult result = m_gameSession.ConsumeLastJudgement();
    if (result != JudgementResult::None)
    {
        m_noteLane.ShowJudgement(result);
    }

    m_noteLane.OnTimer(timerId);
}

// Stops the session/animation/audio engine and quits the message loop.
void MainWindow::OnDestroy()
{
    m_gameSession.Stop();
    m_noteLane.StopAnimating();
    m_audioEngine.Shutdown();
    PostQuitMessage(0);
}

// Repaints the background and the note lane.
void MainWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
    m_noteLane.Draw(hdc, m_gameSession);
    EndPaint(hwnd, &ps);
}

// Opens the chart file picker and loads the chosen chart.
void MainWindow::BrowseForChart(HWND hwnd)
{
    wchar_t szFile[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Rhythm Charts (*.chart)\0*.chart\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"chart";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn))
    {
        return;
    }

    if (m_gameSession.LoadChart(szFile))
    {
        SetWindowTextW(m_hEditChartPath, szFile);
        m_settings.SaveLastChartPath(szFile);
    }
    else
    {
        MessageBoxW(hwnd, L"Couldn't load that chart (check the file and its instrument .wav paths).", kWindowTitle, MB_OK | MB_ICONWARNING);
    }
}
