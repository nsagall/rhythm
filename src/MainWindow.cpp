#include "MainWindow.h"

#include <algorithm>

#include "ColorUtil.h"

using ColorUtil::Darken;
using ColorUtil::Lighten;

namespace
{

constexpr wchar_t kWindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rhythm";

// Where the song library is scraped from, relative to the process's
// working directory - matches the convention the diag-mains already use
// for "Content/..." paths.
constexpr wchar_t kContentRoot[] = L"Content";

constexpr int kControlHeight = 24;
constexpr int kRowTop = 10;
constexpr int kRowLeft = 10;
constexpr int kRowRightMargin = 15;
constexpr int kRefreshButtonWidth = 90;

// The Easy Mode toggle sits on the same header row, immediately to the
// left of the refresh button.
constexpr int kEasyModeToggleWidth = 130;
constexpr int kEasyModeToggleGap = 15;
constexpr int kToggleTrackWidth = 40;
constexpr int kToggleTrackHeight = 20;
constexpr int kToggleKnobRadius = 8;

// The lane (while playing) and the song list (while selecting) both sit
// below this same header row, filling the available space: each grows/
// shrinks with the window but is clamped to a sane range so it never
// becomes an unusably tiny sliver or an absurdly wide single column, and
// stays horizontally centered in whatever room is left over.
constexpr int kToolbarHeight = 46;
constexpr int kLaneMargin = 20;
constexpr int kLaneMinWidth = 200;
constexpr int kLaneMaxWidth = 480;
constexpr int kLaneMinHeight = 320;
constexpr int kSongListMaxWidth = 560;
constexpr int kSongRowHeight = 46;
constexpr int kHintAreaHeight = 34; // reserved below the song list rect for DrawSongList's hint line

// Smallest client area the header row + a minimally-usable lane/list can
// fit in, enforced via WM_GETMINMAXINFO so nothing can overlap/clip.
constexpr int kMinClientWidth = 480;
constexpr int kMinClientHeight = kToolbarHeight + kLaneMargin + kLaneMinHeight + kLaneMargin;

constexpr int IDC_BUTTON_REFRESH = 110;

// Lane 0..3 keyboard bindings, left to right: j, k, l, ; . VK_OEM_1 is the
// Win32 virtual-key constant for the US-layout ';'/':' key - there is no
// named VK_SEMICOLON constant in the Windows headers.
constexpr int kLaneKeys[kLaneCount] = {'J', 'K', 'L', VK_OEM_1};

// Matches NoteLane's palette so the whole window reads as one theme
// instead of a colorful game view floating in plain system-gray chrome.
constexpr COLORREF kWindowBgColor = RGB(15, 11, 30);
constexpr COLORREF kFieldBgColor = RGB(30, 23, 56);
constexpr COLORREF kLabelTextColor = RGB(230, 222, 245);
constexpr COLORREF kRefreshButtonColor = RGB(255, 205, 70);
constexpr COLORREF kSongRowHighlightColor = RGB(56, 219, 255);
constexpr COLORREF kSongRowHighlightTextColor = RGB(10, 10, 20);
constexpr COLORREF kHintTextColor = RGB(150, 140, 175);
constexpr COLORREF kToggleTrackOffColor = kFieldBgColor;
constexpr COLORREF kToggleTrackOnColor = kSongRowHighlightColor;
constexpr COLORREF kToggleKnobColor = RGB(245, 242, 250);

// Custom-paints one owner-drawn button: a rounded, bevelled fill in
// baseColor (darkened while pressed for tactile feedback) with bold dark
// text, matching NoteLane's bar/glyph bevel language.
void DrawGameButton(const DRAWITEMSTRUCT& item, COLORREF baseColor, const wchar_t* label)
{
    HDC hdc = item.hDC;
    RECT rect = item.rcItem;
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    COLORREF fill = pressed ? Darken(baseColor, 40) : baseColor;

    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HPEN oldPen = (HPEN)SelectObject(hdc, nullPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    HBRUSH rimBrush = CreateSolidBrush(Darken(fill, 90));
    SelectObject(hdc, rimBrush);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    DeleteObject(rimBrush);

    HBRUSH fillBrush = CreateSolidBrush(fill);
    SelectObject(hdc, fillBrush);
    RoundRect(hdc, rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - (pressed ? 2 : 4), 12, 12);
    DeleteObject(fillBrush);

    if (!pressed)
    {
        HBRUSH highlightBrush = CreateSolidBrush(Lighten(fill, 70));
        SelectObject(hdc, highlightBrush);
        RECT highlightRect{rect.left + 6, rect.top + 4, rect.right - 6, rect.top + 8};
        FillRect(hdc, &highlightRect, highlightBrush);
        DeleteObject(highlightBrush);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);

    static HFONT buttonFont =
        CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, buttonFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, Darken(fill, 130));
    RECT textRect = rect;
    if (pressed)
    {
        textRect.top += 1;
    }
    DrawTextW(hdc, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

} // namespace

// Binds the game session to this window's audio engine.
MainWindow::MainWindow() : m_gameSession(m_audioEngine)
{
}

// Registers the window class and creates/shows the window.
bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow)
{
    m_windowBrush = CreateSolidBrush(kWindowBgColor);
    m_fieldBrush = CreateSolidBrush(kFieldBgColor);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WindowProcStatic;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_windowBrush;
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
        1024, 840,
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
        case WM_KEYUP:
            OnKeyUp(wParam, lParam);
            return 0;
        case WM_LBUTTONDOWN:
            OnLButtonDown(lParam);
            return 0;
        case WM_TIMER:
            OnTimer(wParam);
            return 0;
        case WM_DRAWITEM:
            OnDrawItem(lParam);
            return TRUE;
        case WM_SIZE:
            OnSize();
            return 0;
        case WM_GETMINMAXINFO:
            OnGetMinMaxInfo(lParam);
            return 0;
        case WM_ERASEBKGND:
            OnEraseBkgnd();
            return 1;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Creates the refresh button and the note lane, then does the initial song scan.
void MainWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;
    m_easyMode = m_settings.LoadEasyMode();

    m_hButtonRefresh = CreateWindowExW(
        0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_REFRESH, nullptr, nullptr
    );

    m_noteLane.Attach(hwnd);
    Layout();
    m_noteLane.StartAnimating();

    if (!m_audioEngine.Initialize())
    {
        MessageBoxW(hwnd, L"Failed to initialize the audio engine.", kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    RescanSongs();

    SetFocus(hwnd);
}

// Repositions the refresh button, and recomputes both the song list rect
// and the note lane rect, to fit the current client area. Both the list and
// the lane are centered, clamped-width columns below the same header row -
// only one is ever visible at a time, but Layout() keeps both current so
// switching screens never needs a re-layout of its own.
void MainWindow::Layout()
{
    RECT client{};
    GetClientRect(m_hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return; // minimized - nothing to lay out
    }

    int refreshLeft = width - kRowRightMargin - kRefreshButtonWidth;
    MoveWindow(m_hButtonRefresh, refreshLeft, kRowTop - 1, kRefreshButtonWidth, kControlHeight + 2, TRUE);

    int toggleRight = refreshLeft - kEasyModeToggleGap;
    int toggleLeft = toggleRight - kEasyModeToggleWidth;
    m_easyModeToggleRect = RECT{toggleLeft, kRowTop - 1, toggleRight, kRowTop - 1 + kControlHeight + 2};

    int listWidth = std::min(std::max(width - 2 * kLaneMargin, kLaneMinWidth), kSongListMaxWidth);
    int listLeft = (width - listWidth) / 2;
    int listTop = kToolbarHeight + kLaneMargin;
    // Leaves room below the list for DrawSongList's hint line, which is
    // drawn just under this rect's bottom edge - without this, a short
    // window clips the hint against (or past) the actual window edge.
    int listBottom = std::max(listTop + kLaneMinHeight, height - kLaneMargin - kHintAreaHeight);
    m_songListRect = RECT{listLeft, listTop, listLeft + listWidth, listBottom};

    int laneWidth = std::min(std::max(width - 2 * kLaneMargin, kLaneMinWidth), kLaneMaxWidth);
    int laneLeft = (width - laneWidth) / 2;
    int laneTop = kToolbarHeight + kLaneMargin;
    int laneBottom = std::max(laneTop + kLaneMinHeight, height - kLaneMargin);
    RECT laneRect{laneLeft, laneTop, laneLeft + laneWidth, laneBottom};
    m_noteLane.SetLaneRect(laneRect);

    InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Re-runs Layout() for the new client size.
void MainWindow::OnSize()
{
    Layout();
}

// Keeps the window from being resized smaller than the lane/list can fit
// in. ptMinTrackSize is a whole-window size (including the title bar and
// borders), so the desired minimum client area has to be grown by however
// much chrome this window style adds before being reported.
void MainWindow::OnGetMinMaxInfo(LPARAM lParam)
{
    RECT rect{0, 0, kMinClientWidth, kMinClientHeight};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    MINMAXINFO& info = *reinterpret_cast<MINMAXINFO*>(lParam);
    info.ptMinTrackSize.x = rect.right - rect.left;
    info.ptMinTrackSize.y = rect.bottom - rect.top;
}

// Routes a WM_COMMAND control ID to the refresh handler.
void MainWindow::OnCommand(HWND hwnd, int controlId)
{
    if (controlId == IDC_BUTTON_REFRESH)
    {
        RescanSongs(/*reportValidationErrors=*/true);
        // The button would otherwise keep keyboard focus and steal lane/list keystrokes.
        SetFocus(hwnd);
    }
}

// On the song list: Up/Down moves the highlight, any other key (besides a
// pure modifier or Left/Right, neither of which mean anything for a single
// vertical list) chooses the highlighted song. While playing: registers a
// press on a non-repeated key-down for one of the lane keys (j/k/l/;).
void MainWindow::OnKeyDown(WPARAM key, LPARAM flags)
{
    bool isAutoRepeat = (flags & (1 << 30)) != 0;
    if (isAutoRepeat)
    {
        return;
    }

    if (m_screen == UiScreen::SongSelect)
    {
        if (static_cast<int>(key) == VK_ESCAPE)
        {
            // Checked before the empty-list early-return below, so Escape
            // still quits even when there's nothing to select.
            DestroyWindow(m_hwnd);
            return;
        }

        if (m_songs.empty())
        {
            return;
        }

        int vk = static_cast<int>(key);
        if (vk == VK_UP)
        {
            m_selectedSongIndex = std::max(m_selectedSongIndex - 1, 0);
            InvalidateRect(m_hwnd, &m_songListRect, FALSE);
            return;
        }
        if (vk == VK_DOWN)
        {
            m_selectedSongIndex = std::min(m_selectedSongIndex + 1, static_cast<int>(m_songs.size()) - 1);
            InvalidateRect(m_hwnd, &m_songListRect, FALSE);
            return;
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN)
        {
            return;
        }

        ChooseSong(m_selectedSongIndex);
        return;
    }

    if (static_cast<int>(key) == VK_ESCAPE)
    {
        QuitToSongSelect();
        return;
    }

    if (static_cast<int>(key) == 'D')
    {
        m_noteLane.ToggleDebugOverlay();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (static_cast<int>(key) == kLaneKeys[lane])
        {
            RegisterPress(lane);
            return;
        }
    }
}

// Registers a release on key-up for one of the lane keys (j/k/l/;).
void MainWindow::OnKeyUp(WPARAM key, LPARAM /*flags*/)
{
    // WM_KEYUP never carries synthetic auto-repeat events (Windows only
    // auto-repeats WM_KEYDOWN), so every key-up handled here is a real
    // physical release - no debouncing needed.
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (static_cast<int>(key) == kLaneKeys[lane])
        {
            RegisterRelease(lane);
            return;
        }
    }
}

// On the song list, clicking the Easy Mode toggle flips it; clicking a row
// chooses that song immediately.
void MainWindow::OnLButtonDown(LPARAM lParam)
{
    if (m_screen != UiScreen::SongSelect)
    {
        return;
    }

    POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};

    if (PtInRect(&m_easyModeToggleRect, pt))
    {
        m_easyMode = !m_easyMode;
        m_settings.SaveEasyMode(m_easyMode);
        InvalidateRect(m_hwnd, &m_easyModeToggleRect, FALSE);
        return;
    }

    int index = HitTestSongList(pt);
    if (index >= 0)
    {
        m_selectedSongIndex = index;
        ChooseSong(index);
    }
}

// Sends a lane press to the game session and reflects any judgement in the
// note lane - or, if this lane has no note to press right now at all (no
// chart loaded, count-in/intro/solo playing, or this clip simply never uses
// this lane), shows the same miss feedback directly, since OnPress itself
// would otherwise just silently ignore the tap.
void MainWindow::RegisterPress(int lane)
{
    // Must run before IsLaneJudgeable - a press landing right on the
    // count-in's own boundary needs the phase to have already flipped to
    // Learning, or IsLaneJudgeable rejects it outright regardless of
    // tolerance (see CatchUpCountIn's own comment).
    m_gameSession.CatchUpCountIn();
    if (!m_gameSession.IsLaneJudgeable(lane))
    {
        m_noteLane.ShowJudgement(JudgementResult::Miss, lane, false);
        return;
    }

    m_gameSession.OnPress(lane);
    DrainJudgements();
}

// Sends a lane release to the game session and reflects any judgement in the note lane.
void MainWindow::RegisterRelease(int lane)
{
    m_gameSession.OnRelease(lane);
    DrainJudgements();
}

// See the header's own comment.
void MainWindow::DrainJudgements()
{
    for (const GameSession::JudgementEvent& event : m_gameSession.ConsumeJudgementEvents())
    {
        m_noteLane.ShowJudgement(event.result, event.lane, event.passing);
    }
}

// Advances the game session and reflects any judgement in the note lane.
// Once a song completes while it's the visible screen, switches back to the
// song list - playback itself (whatever's still looping) is left running
// untouched, exactly as it already would without this UI on top of it.
void MainWindow::OnTimer(WPARAM timerId)
{
    m_gameSession.Update();
    DrainJudgements();

    if (m_screen == UiScreen::Playing && m_gameSession.Phase() == GamePhase::Complete)
    {
        m_screen = UiScreen::SongSelect;
        ShowWindow(m_hButtonRefresh, SW_SHOW);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    m_noteLane.OnTimer(timerId);
}

// Bails out of the current song (Esc while Playing) and returns to the song
// list - same screen switch as a natural completion, but also stops the
// session outright rather than leaving a finished clip's loop running.
void MainWindow::QuitToSongSelect()
{
    if (m_screen != UiScreen::Playing)
    {
        return;
    }

    m_gameSession.Stop();
    m_screen = UiScreen::SongSelect;
    ShowWindow(m_hButtonRefresh, SW_SHOW);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Stops the session/animation/audio engine and quits the message loop.
void MainWindow::OnDestroy()
{
    m_gameSession.Stop();
    m_noteLane.StopAnimating();
    m_audioEngine.Shutdown();
    DeleteObject(m_windowBrush);
    DeleteObject(m_fieldBrush);
    if (m_backBufferBitmap)
    {
        DeleteObject(m_backBufferBitmap);
        DeleteDC(m_backBufferDC);
    }
    PostQuitMessage(0);
}

// (Re)creates the off-screen back buffer to match the given size, if it doesn't already match.
void MainWindow::EnsureBackBuffer(HDC referenceHdc, int width, int height)
{
    if (m_backBufferBitmap && width == m_backBufferWidth && height == m_backBufferHeight)
    {
        return;
    }
    if (m_backBufferBitmap)
    {
        DeleteObject(m_backBufferBitmap);
        DeleteDC(m_backBufferDC);
    }
    m_backBufferDC = CreateCompatibleDC(referenceHdc);
    m_backBufferBitmap = CreateCompatibleBitmap(referenceHdc, width, height);
    SelectObject(m_backBufferDC, m_backBufferBitmap);
    m_backBufferWidth = width;
    m_backBufferHeight = height;
}

// Repaints the background and whichever screen is current into the
// off-screen back buffer, then blits the finished frame to the screen in
// one copy - the window only ever shows a fully-drawn frame, never one
// caught mid-draw.
void MainWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    if (width > 0 && height > 0)
    {
        EnsureBackBuffer(hdc, width, height);
        FillRect(m_backBufferDC, &client, m_windowBrush);
        if (m_screen == UiScreen::SongSelect)
        {
            DrawSongList(m_backBufferDC);
            DrawEasyModeToggle(m_backBufferDC);
        }
        else
        {
            m_noteLane.Draw(m_backBufferDC, m_gameSession);
        }
        BitBlt(hdc, 0, 0, width, height, m_backBufferDC, 0, 0, SRCCOPY);
    }

    EndPaint(hwnd, &ps);
}

// See declaration - deliberately does not erase; OnPaint's back buffer
// already repaints the entire client area every time.
void MainWindow::OnEraseBkgnd()
{
}

// Custom-paints the refresh button via DrawGameButton.
void MainWindow::OnDrawItem(LPARAM lParam)
{
    const DRAWITEMSTRUCT& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
    if (item.CtlID == IDC_BUTTON_REFRESH)
    {
        DrawGameButton(item, kRefreshButtonColor, L"Refresh");
    }
}

// Re-scrapes Content\ for songs, keeping the currently-highlighted song
// selected if it's still found, otherwise falling back to the last-played
// chart from Settings, otherwise index 0.
void MainWindow::RescanSongs(bool reportValidationErrors)
{
    std::wstring preferredPath;
    if (m_selectedSongIndex >= 0 && m_selectedSongIndex < static_cast<int>(m_songs.size()))
    {
        preferredPath = m_songs[m_selectedSongIndex].chartPath;
    }
    else
    {
        preferredPath = m_settings.LoadLastChartPath();
    }

    std::vector<std::wstring> validationErrors;
    m_songs = SongLibrary::Scrape(kContentRoot, reportValidationErrors ? &validationErrors : nullptr);

    m_selectedSongIndex = m_songs.empty() ? -1 : 0;
    if (!preferredPath.empty())
    {
        for (size_t i = 0; i < m_songs.size(); ++i)
        {
            if (m_songs[i].chartPath == preferredPath)
            {
                m_selectedSongIndex = static_cast<int>(i);
                break;
            }
        }
    }

    InvalidateRect(m_hwnd, &m_songListRect, FALSE);

    if (!validationErrors.empty())
    {
        std::wstring message = L"The following charts failed validation and were left out of the list:\r\n";
        for (const std::wstring& error : validationErrors)
        {
            message += L"\r\n" + error + L"\r\n";
        }
        MessageBoxW(m_hwnd, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
    }
}

// Loads and starts the given song and switches to the Playing screen -
// shows an error dialog instead if the chart fails to load.
void MainWindow::ChooseSong(int index)
{
    if (index < 0 || index >= static_cast<int>(m_songs.size()))
    {
        return;
    }

    std::wstring loadError;
    if (!m_gameSession.LoadChart(m_songs[index].chartPath, m_easyMode, loadError))
    {
        std::wstring message = L"That chart couldn't be loaded:\r\n\r\n" + loadError;
        MessageBoxW(m_hwnd, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    m_settings.SaveLastChartPath(m_songs[index].chartPath);
    m_gameSession.Start();
    m_screen = UiScreen::Playing;
    ShowWindow(m_hButtonRefresh, SW_HIDE);
    SetFocus(m_hwnd); // a native control would otherwise keep keyboard focus and steal lane keystrokes
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Paints the song list: a header, one row per scraped song (the
// currently-highlighted one picked out from the rest), and a hint about how
// to choose one.
void MainWindow::DrawSongList(HDC hdc)
{
    RECT rect = m_songListRect;
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    static HFONT titleFont =
        CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    static HFONT rowFont =
        CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    static HFONT hintFont =
        CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    SetBkMode(hdc, TRANSPARENT);

    RECT headerRect{kRowLeft, kRowTop, kRowLeft + 260, kRowTop + kControlHeight + 6};
    HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
    SetTextColor(hdc, kLabelTextColor);
    DrawTextW(hdc, L"Choose a Song", -1, &headerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, rowFont);

    if (m_songs.empty())
    {
        SetTextColor(hdc, kLabelTextColor);
        RECT emptyRect = rect;
        DrawTextW(hdc, L"No songs found in Content\\ - add a song folder, then press Refresh.", -1, &emptyRect,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        return;
    }

    for (size_t i = 0; i < m_songs.size(); ++i)
    {
        RECT rowRect{rect.left, rect.top + static_cast<int>(i) * kSongRowHeight, rect.right,
                     rect.top + static_cast<int>(i + 1) * kSongRowHeight};
        if (rowRect.top >= rect.bottom)
        {
            break;
        }

        bool selected = (static_cast<int>(i) == m_selectedSongIndex);
        if (selected)
        {
            HBRUSH highlightBrush = CreateSolidBrush(kSongRowHighlightColor);
            HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            HBRUSH oldRowBrush = (HBRUSH)SelectObject(hdc, highlightBrush);
            RoundRect(hdc, rowRect.left, rowRect.top + 2, rowRect.right, rowRect.bottom - 2, 10, 10);
            SelectObject(hdc, oldRowBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(highlightBrush);
        }
        else
        {
            FillRect(hdc, &rowRect, m_fieldBrush);
        }

        RECT textRect = rowRect;
        textRect.left += 16;
        textRect.right -= 16;
        SetTextColor(hdc, selected ? kSongRowHighlightTextColor : kLabelTextColor);
        DrawTextW(hdc, m_songs[i].title.c_str(), -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    RECT hintRect{rect.left, rect.bottom + 10, rect.right, rect.bottom + 34};
    SelectObject(hdc, hintFont);
    SetTextColor(hdc, kHintTextColor);
    DrawTextW(hdc, L"Click a song, or use \x2191/\x2193 and any other key to start it.", -1, &hintRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, oldFont);
}

// Custom-paints the Easy Mode toggle: a label plus a rounded track with a
// circular knob, positioned left (off) or right (on) - no native control,
// matching the song list's own hand-drawn/hand-hit-tested style.
void MainWindow::DrawEasyModeToggle(HDC hdc)
{
    RECT rect = m_easyModeToggleRect;
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return;
    }

    static HFONT labelFont =
        CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    int centerY = (rect.top + rect.bottom) / 2;
    RECT trackRect{rect.right - kToggleTrackWidth, centerY - kToggleTrackHeight / 2, rect.right,
                    centerY + kToggleTrackHeight / 2};

    HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));

    HBRUSH trackBrush = CreateSolidBrush(m_easyMode ? kToggleTrackOnColor : kToggleTrackOffColor);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, trackBrush);
    RoundRect(hdc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom, kToggleTrackHeight,
              kToggleTrackHeight);
    SelectObject(hdc, oldBrush);
    DeleteObject(trackBrush);

    int knobCenterX = m_easyMode ? trackRect.right - kToggleTrackHeight / 2 : trackRect.left + kToggleTrackHeight / 2;
    HBRUSH knobBrush = CreateSolidBrush(kToggleKnobColor);
    SelectObject(hdc, knobBrush);
    Ellipse(hdc, knobCenterX - kToggleKnobRadius, centerY - kToggleKnobRadius, knobCenterX + kToggleKnobRadius,
            centerY + kToggleKnobRadius);
    SelectObject(hdc, oldBrush);
    DeleteObject(knobBrush);

    SelectObject(hdc, oldPen);

    RECT labelRect{rect.left, rect.top, trackRect.left - 8, rect.bottom};
    HFONT oldFont = (HFONT)SelectObject(hdc, labelFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kLabelTextColor);
    DrawTextW(hdc, L"Easy Mode", -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

// Returns the song list row index under the given client-space point, or -1 if none.
int MainWindow::HitTestSongList(POINT pt) const
{
    if (pt.x < m_songListRect.left || pt.x > m_songListRect.right || pt.y < m_songListRect.top ||
        pt.y >= m_songListRect.bottom)
    {
        return -1;
    }

    int row = (pt.y - m_songListRect.top) / kSongRowHeight;
    if (row < 0 || row >= static_cast<int>(m_songs.size()))
    {
        return -1;
    }
    return row;
}
