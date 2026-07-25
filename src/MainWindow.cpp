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
constexpr int kTapButtonTop = 470;
constexpr int kTapButtonWidth = 200;
constexpr int kTapButtonHeight = 60;

constexpr int IDC_EDIT_CHARTPATH = 101;
constexpr int IDC_BUTTON_BROWSE = 110;
constexpr int IDC_BUTTON_START = 120;
constexpr int IDC_BUTTON_TAP = 130;

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

// Subclass proc for the Tap button: registers the tap on WM_LBUTTONDOWN (press) instead of waiting for BN_CLICKED (release).
LRESULT CALLBACK MainWindow::TapButtonProcStatic(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // The standard BUTTON class has CS_DBLCLKS set, so a second click that
    // lands within the system's double-click time (rapid taps for a fast
    // pattern easily do) arrives as WM_LBUTTONDBLCLK instead of a second
    // WM_LBUTTONDOWN - without handling it too, every other fast tap would
    // be silently dropped.
    if (self && (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK))
    {
        self->RegisterTap();
    }

    if (self && self->m_originalTapButtonProc)
    {
        return CallWindowProcW(self->m_originalTapButtonProc, hwnd, message, wParam, lParam);
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

    m_hButtonTap = CreateWindowExW(
        0, L"BUTTON", L"Tap",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        kLaneLeft, kTapButtonTop, kTapButtonWidth, kTapButtonHeight,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_TAP, nullptr, nullptr
    );
    SendMessageW(m_hButtonTap, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowLongPtrW(m_hButtonTap, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_originalTapButtonProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(m_hButtonTap, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWindow::TapButtonProcStatic)));

    if (!m_audioEngine.Initialize())
    {
        MessageBoxW(hwnd, L"Failed to initialize the audio engine.", kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    std::wstring lastChartPath = m_settings.LoadLastChartPath();
    if (!lastChartPath.empty())
    {
        std::wstring loadError;
        if (m_gameSession.LoadChart(lastChartPath, loadError))
        {
            SetWindowTextW(m_hEditChartPath, lastChartPath.c_str());
        }
        else
        {
            std::wstring message = L"The last chart you had open is no longer valid:\r\n\r\n" + loadError +
                                    L"\r\n\r\nPlease pick a different chart.";
            MessageBoxW(hwnd, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
        }
    }

    SetFocus(hwnd);
}

// Routes a WM_COMMAND control ID to the Browse/Start/Tap handler.
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
        // Native buttons treat Space/Enter as "activate me" while focused,
        // which would otherwise steal every tap keystroke after this click.
        SetFocus(hwnd);
        return;
    }

    if (controlId == IDC_BUTTON_TAP)
    {
        // The tap itself is registered on press, in TapButtonProcStatic (WM_LBUTTONDOWN) -
        // BN_CLICKED only fires on release, which would add the user's press-to-release
        // dwell time as extra latency. This just restores focus for subsequent spacebar taps.
        SetFocus(hwnd);
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

    RegisterTap();
}

// Sends a tap to the game session and reflects any judgement in the note lane.
void MainWindow::RegisterTap()
{
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

    SetFocus(hwnd); // the Browse button would otherwise keep keyboard focus and steal tap keystrokes

    std::wstring loadError;
    if (m_gameSession.LoadChart(szFile, loadError))
    {
        SetWindowTextW(m_hEditChartPath, szFile);
        m_settings.SaveLastChartPath(szFile);
    }
    else
    {
        std::wstring message = L"That chart couldn't be loaded:\r\n\r\n" + loadError + L"\r\n\r\nPlease pick a different file.";
        MessageBoxW(hwnd, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
    }
}
