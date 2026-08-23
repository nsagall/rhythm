#include "MainWindow.h"

#include <algorithm>
#include <filesystem>
#include <mmsystem.h>

#include "ColorUtil.h"
#include "Colors.h"

using ColorUtil::Darken;
using ColorUtil::Lighten;
using GameColors::c_AssignButtonColor;
using GameColors::c_FieldBgColor;
using GameColors::c_HintTextColor;
using GameColors::c_LabelTextColor;
using GameColors::c_RefreshButtonColor;
using GameColors::c_SongRowHighlightColor;
using GameColors::c_SongRowHighlightTextColor;
using GameColors::c_ToggleKnobColor;
using GameColors::c_ToggleTrackOffColor;
using GameColors::c_ToggleTrackOnColor;
using GameColors::c_WindowBgColor;

namespace
{

constexpr wchar_t c_WindowClassName[] = L"RhythmWindowClass";
constexpr wchar_t c_WindowTitle[] = L"Rhythm";

// Where the song library is scraped from, relative to the process's
// working directory - matches the convention the diag-mains already use
// for "Content/..." paths.
constexpr wchar_t c_ContentRoot[] = L"Content";

constexpr int c_ControlHeight = 24;
constexpr int c_RowTop = 10;
constexpr int c_RowLeft = 10;
constexpr int c_RowRightMargin = 15;
constexpr int c_RefreshButtonWidth = 90;

// The Easy Mode toggle sits on the same header row, immediately to the
// left of the refresh button.
constexpr int c_EasyModeToggleWidth = 130;
constexpr int c_EasyModeToggleGap = 15;
constexpr int c_ToggleTrackWidth = 40;
constexpr int c_ToggleTrackHeight = 20;
constexpr int c_ToggleKnobRadius = 8;

// The Assign Inputs button sits on the same header row, immediately to the
// left of the Easy Mode toggle.
constexpr int c_AssignButtonWidth = 130;
constexpr int c_AssignButtonGap = 15;

// The lane (while playing) and the song list (while selecting) both sit
// below this same header row, filling the available space: each grows/
// shrinks with the window but is clamped to a sane range so it never
// becomes an unusably tiny sliver or an absurdly wide single column, and
// stays horizontally centered in whatever room is left over.
constexpr int c_ToolbarHeight = 46;
constexpr int c_LaneMargin = 20;
constexpr int c_LaneMinWidth = 200;
constexpr int c_LaneMaxWidth = 480;
constexpr int c_LaneMinHeight = 320;
constexpr int c_SongListMaxWidth = 560;
constexpr int c_SongRowHeight = 46;

// The hits meter panel sits to the lane's right as its own small, separate
// widget - fixed-size (unlike the lane itself) since it only ever shows a
// single fill bar, no need to grow with the window. A wide gap and a
// height shorter than the lane's own (vertically centered - see Layout())
// keep it reading as clearly detached from the playfield rather than an
// extra column welded onto it.
constexpr int c_HitsMeterWidth = 16;
constexpr int c_HitsMeterGap = 34;
constexpr double c_HitsMeterHeightFraction = 0.5;
constexpr int c_HintAreaHeight = 34; // reserved below the song list rect for DrawSongList's hint line

// Smallest client area the header row + a minimally-usable lane/list can
// fit in, enforced via WM_GETMINMAXINFO so nothing can overlap/clip.
constexpr int c_MinClientWidth = 480;
constexpr int c_MinClientHeight = c_ToolbarHeight + c_LaneMargin + c_LaneMinHeight + c_LaneMargin;

constexpr int IDC_BUTTON_REFRESH = 110;
constexpr int IDC_BUTTON_ASSIGN = 111;

// Minimum MIDI velocity (0-127) a note-on must carry to count as a real
// press, in gameplay or during input capture alike - a sensitive
// pad/keyboard controller can send genuine note-on messages for a barely-
// there touch, and those shouldn't register as a lane hit (or get captured
// as a binding) any more than an accidental brush of a key would. Below
// this, OnMidiData drops the message entirely rather than treating it as
// either a press or a release.
constexpr BYTE c_MinMidiPressVelocity = 20;

// Returns value formatted with thousands separators (e.g. 12345 -> "12,345") -
// scores read a lot more easily this way than as a bare digit run.
std::wstring FormatScoreWithCommas(int value)
{
    std::wstring digits = std::to_wstring(value);
    std::wstring result;
    int sinceComma = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (sinceComma == 3)
        {
            result.push_back(L',');
            sinceComma = 0;
        }
        result.push_back(*it);
        ++sinceComma;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

// Returns the stable key a song's high score list is saved under: its
// content folder's own name (e.g. "Content\MySong\MySong.chart" -> "MySong") -
// see SongLibrary's own comment on one folder per song. More stable across
// machines/working directories than the full chartPath itself, which is all
// Settings::LoadLastChartPath otherwise keys off of.
std::wstring SongKeyForChartPath(const std::wstring& chartPath)
{
    return std::filesystem::path(chartPath).parent_path().filename().wstring();
}

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
    m_windowBrush = CreateSolidBrush(c_WindowBgColor);
    m_fieldBrush = CreateSolidBrush(c_FieldBgColor);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WindowProcStatic;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_windowBrush;
    wc.lpszClassName = c_WindowClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    HWND hwnd = CreateWindowExW(
        0,
        c_WindowClassName,
        c_WindowTitle,
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
        case MM_MIM_DATA:
            OnMidiData(wParam, lParam);
            return 0;
        case WM_ACTIVATE:
            OnActivate(wParam);
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

// Creates the refresh/Assign Inputs buttons and the note lane, loads saved
// lane bindings and opens any connected MIDI devices, then does the initial
// song scan.
void MainWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;
    m_easyMode = m_settings.LoadEasyMode();
    m_laneBindings.Load(m_settings);

    m_hButtonRefresh = CreateWindowExW(
        0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_REFRESH, nullptr, nullptr
    );

    m_hButtonAssign = CreateWindowExW(
        0, L"BUTTON", L"Assign Inputs",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0,
        hwnd, (HMENU)(INT_PTR)IDC_BUTTON_ASSIGN, nullptr, nullptr
    );

    m_noteLane.Attach(hwnd);
    Layout();
    m_noteLane.StartAnimating();

    if (!m_audioEngine.Initialize())
    {
        MessageBoxW(hwnd, L"Failed to initialize the audio engine.", c_WindowTitle, MB_OK | MB_ICONWARNING);
    }
    else
    {
        // Missing/unsupported files are a silent no-op (see the header's
        // own comment) - these are optional flourishes, not required
        // content the way a chart's own stems are.
        m_sfxMultiplierUp = m_audioEngine.LoadSfx(std::wstring(c_ContentRoot) + L"\\sfx\\multiplier_up.wav");
        m_sfxStreakBroken = m_audioEngine.LoadSfx(std::wstring(c_ContentRoot) + L"\\sfx\\streak_broken.wav");
    }

    // Zero connected MIDI devices is the normal case (keyboard-only play) -
    // no failure dialog, matching AudioEngine's own soft-failure style above.
    m_midiInput.OpenAll(hwnd);

    RescanSongs();

    SetFocus(hwnd);
}

// Repositions the refresh and Assign Inputs buttons, and recomputes both the
// song list rect and the note lane rect, to fit the current client area. Both the list and
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

    int refreshLeft = width - c_RowRightMargin - c_RefreshButtonWidth;
    MoveWindow(m_hButtonRefresh, refreshLeft, c_RowTop - 1, c_RefreshButtonWidth, c_ControlHeight + 2, TRUE);

    int toggleRight = refreshLeft - c_EasyModeToggleGap;
    int toggleLeft = toggleRight - c_EasyModeToggleWidth;
    m_easyModeToggleRect = RECT{toggleLeft, c_RowTop - 1, toggleRight, c_RowTop - 1 + c_ControlHeight + 2};

    int assignRight = toggleLeft - c_AssignButtonGap;
    int assignLeft = assignRight - c_AssignButtonWidth;
    MoveWindow(m_hButtonAssign, assignLeft, c_RowTop - 1, c_AssignButtonWidth, c_ControlHeight + 2, TRUE);

    int listWidth = std::min(std::max(width - 2 * c_LaneMargin, c_LaneMinWidth), c_SongListMaxWidth);
    int listLeft = (width - listWidth) / 2;
    int listTop = c_ToolbarHeight + c_LaneMargin;
    // Leaves room below the list for DrawSongList's hint line, which is
    // drawn just under this rect's bottom edge - without this, a short
    // window clips the hint against (or past) the actual window edge.
    int listBottom = std::max(listTop + c_LaneMinHeight, height - c_LaneMargin - c_HintAreaHeight);
    m_songListRect = RECT{listLeft, listTop, listLeft + listWidth, listBottom};

    // The lane and the hits meter beside it are laid out as one combined,
    // centered unit - the lane itself still clamps to the same
    // min/max width it always has, just computed against the width left
    // over once the meter's own fixed footprint is reserved, so the pair
    // together stays centered instead of the lane alone.
    int hitsMeterFootprint = c_HitsMeterGap + c_HitsMeterWidth;
    int laneAvailableWidth = width - 2 * c_LaneMargin - hitsMeterFootprint;
    int laneWidth = std::min(std::max(laneAvailableWidth, c_LaneMinWidth), c_LaneMaxWidth);
    int combinedWidth = laneWidth + hitsMeterFootprint;
    int laneLeft = (width - combinedWidth) / 2;
    int laneTop = c_ToolbarHeight + c_LaneMargin;
    int laneBottom = std::max(laneTop + c_LaneMinHeight, height - c_LaneMargin);
    RECT laneRect{laneLeft, laneTop, laneLeft + laneWidth, laneBottom};
    m_noteLane.SetLaneRect(laneRect);

    int laneHeight = laneBottom - laneTop;
    int hitsMeterHeight = static_cast<int>(laneHeight * c_HitsMeterHeightFraction);
    int hitsMeterTop = laneTop + (laneHeight - hitsMeterHeight) / 2;
    RECT hitsMeterRect{laneRect.right + c_HitsMeterGap, hitsMeterTop, laneRect.right + hitsMeterFootprint,
                        hitsMeterTop + hitsMeterHeight};
    m_noteLane.SetHitsMeterRect(hitsMeterRect);

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
    RECT rect{0, 0, c_MinClientWidth, c_MinClientHeight};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    MINMAXINFO& info = *reinterpret_cast<MINMAXINFO*>(lParam);
    info.ptMinTrackSize.x = rect.right - rect.left;
    info.ptMinTrackSize.y = rect.bottom - rect.top;
}

// Routes a WM_COMMAND control ID to the refresh handler, or to
// BeginCapture() for the Assign Inputs button.
void MainWindow::OnCommand(HWND hwnd, int controlId)
{
    if (controlId == IDC_BUTTON_REFRESH)
    {
        RescanSongs(/*reportValidationErrors=*/true);
        // The button would otherwise keep keyboard focus and steal lane/list keystrokes.
        SetFocus(hwnd);
    }
    else if (controlId == IDC_BUTTON_ASSIGN)
    {
        BeginCapture();
        SetFocus(hwnd);
    }
}

// While capturing an input assignment, every key goes to
// HandleCaptureKeyDown instead. Otherwise, on the song list: Up/Down moves
// the highlight, any other key (besides a pure modifier or Left/Right,
// neither of which mean anything for a single vertical list) chooses the
// highlighted song. While playing: registers a press on a non-repeated
// key-down for whichever lane m_laneBindings resolves the key to (its
// default key, or a custom binding).
void MainWindow::OnKeyDown(WPARAM key, LPARAM flags)
{
    bool isAutoRepeat = (flags & (1 << 30)) != 0;
    if (isAutoRepeat)
    {
        return;
    }

    if (m_captureLane != -1)
    {
        HandleCaptureKeyDown(static_cast<int>(key));
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

    if (static_cast<int>(key) == VK_SPACE)
    {
        TogglePause();
        return;
    }

    if (static_cast<int>(key) == 'D')
    {
        m_noteLane.ToggleDebugOverlay();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (m_gameSession.IsPaused())
    {
        return; // lane keys do nothing while paused - only Space (above) does
    }

    int lane = m_laneBindings.LaneForKey(static_cast<int>(key));
    if (lane != -1)
    {
        RegisterPress(lane);
    }
}

// Registers a release on key-up for whichever lane m_laneBindings resolves
// the key to (its default key, or a custom binding).
void MainWindow::OnKeyUp(WPARAM key, LPARAM /*flags*/)
{
    // WM_KEYUP never carries synthetic auto-repeat events (Windows only
    // auto-repeats WM_KEYDOWN), so every key-up handled here is a real
    // physical release - no debouncing needed.
    int lane = m_laneBindings.LaneForKey(static_cast<int>(key));
    if (lane != -1)
    {
        RegisterRelease(lane);
    }
}

// See the header's own comment.
void MainWindow::HandleCaptureKeyDown(int vkCode)
{
    if (vkCode == VK_ESCAPE)
    {
        CancelCapture();
        return;
    }

    if (vkCode == VK_SPACE || vkCode == 'D' || m_laneBindings.IsDefaultKey(vkCode))
    {
        m_captureRejectionMessage = L"That key is already used elsewhere - press a different one.";
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    m_laneBindings.SetCustom(m_captureLane, InputBinding{InputKind::Keyboard, vkCode}, m_settings);
    AdvanceCapture();
}

// See the header's own comment. wParam (the HMIDIIN handle the event came
// from) is deliberately unused - lane matching only cares about the note
// number, not which device sent it (see MidiInputManager's own comment on
// "any input device").
void MainWindow::OnMidiData(WPARAM /*wParam*/, LPARAM lParam)
{
    MidiInputManager::ShortMessage message = MidiInputManager::Unpack(lParam);
    int command = message.status & 0xF0;
    if (command != 0x90 && command != 0x80)
    {
        return; // only note-on/note-off matter for lane triggers
    }

    bool isNoteOnRaw = (command == 0x90 && message.data2 > 0);
    if (isNoteOnRaw && message.data2 < c_MinMidiPressVelocity)
    {
        return; // too light to count as a real press - not a press or a release
    }

    bool isNoteOn = isNoteOnRaw;
    int note = message.data1;

    if (m_captureLane != -1)
    {
        if (isNoteOn)
        {
            m_laneBindings.SetCustom(m_captureLane, InputBinding{InputKind::MidiNote, note}, m_settings);
            AdvanceCapture();
        }
        return;
    }

    if (m_screen != UiScreen::Playing)
    {
        return;
    }

    int lane = m_laneBindings.LaneForMidiNote(note);
    if (lane == -1)
    {
        return;
    }

    if (isNoteOn)
    {
        if (!m_gameSession.IsPaused())
        {
            RegisterPress(lane);
        }
    }
    else
    {
        RegisterRelease(lane);
    }
}

// See the header's own comment.
void MainWindow::OnGamepadButton(WORD button, bool pressed)
{
    if (m_captureLane != -1)
    {
        if (pressed)
        {
            m_laneBindings.SetCustom(m_captureLane, InputBinding{InputKind::Gamepad, button}, m_settings);
            AdvanceCapture();
        }
        return;
    }

    if (m_screen != UiScreen::Playing)
    {
        return;
    }

    int lane = m_laneBindings.LaneForGamepadButton(button);
    if (lane == -1)
    {
        return;
    }

    if (pressed)
    {
        if (!m_gameSession.IsPaused())
        {
            RegisterPress(lane);
        }
    }
    else
    {
        RegisterRelease(lane);
    }
}

// Pauses on deactivation (Alt-Tab, clicking another window) while Playing.
void MainWindow::OnActivate(WPARAM wParam)
{
    if (LOWORD(wParam) != WA_INACTIVE || m_screen != UiScreen::Playing)
    {
        return;
    }
    m_gameSession.Pause();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// See the header comment.
void MainWindow::TogglePause()
{
    if (m_screen != UiScreen::Playing)
    {
        return;
    }
    if (m_gameSession.IsPaused())
    {
        m_gameSession.Resume();
    }
    else
    {
        m_gameSession.Pause();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// See the header's own comment.
void MainWindow::BeginCapture()
{
    if (m_screen != UiScreen::SongSelect)
    {
        return; // the Assign button is hidden outside SongSelect - defensive only
    }

    m_captureLane = 0;
    m_captureRejectionMessage.clear();
    EnableWindow(m_hButtonAssign, FALSE);
    EnableWindow(m_hButtonRefresh, FALSE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// See the header's own comment.
void MainWindow::AdvanceCapture()
{
    ++m_captureLane;
    m_captureRejectionMessage.clear();
    if (m_captureLane >= c_LaneCount)
    {
        m_captureLane = -1;
        EnableWindow(m_hButtonAssign, TRUE);
        EnableWindow(m_hButtonRefresh, TRUE);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// See the header's own comment.
void MainWindow::CancelCapture()
{
    m_captureLane = -1;
    m_captureRejectionMessage.clear();
    EnableWindow(m_hButtonAssign, TRUE);
    EnableWindow(m_hButtonRefresh, TRUE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// On the song list, clicking the Easy Mode toggle flips it; clicking a row
// chooses that song immediately.
void MainWindow::OnLButtonDown(LPARAM lParam)
{
    if (m_screen != UiScreen::SongSelect || m_captureLane != -1)
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
// would otherwise just silently ignore the tap. In between those two cases,
// TryBufferEarlyPress handles a press that's early only because the section
// owning its note hasn't begun yet - see its own comment.
void MainWindow::RegisterPress(int lane)
{
    // Must run before IsLaneJudgeable - a press landing right on the
    // count-in's own boundary needs the phase to have already flipped to
    // Learning, or IsLaneJudgeable rejects it outright regardless of
    // tolerance (see CatchUpCountIn's own comment).
    m_gameSession.CatchUpCountIn();
    if (!m_gameSession.IsLaneJudgeable(lane))
    {
        if (!m_gameSession.TryBufferEarlyPress(lane))
        {
            m_noteLane.ShowJudgement(JudgementResult::Miss, lane, false, /*precise=*/true);
        }
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
        m_noteLane.ShowJudgement(event.result, event.lane, event.passing, event.precise);
    }
    for (const GameSession::HudChangeEvent& event : m_gameSession.ConsumeHudChangeEvents())
    {
        m_noteLane.ShowHudValueChanged(event.field, event.newValue);
    }
    for (GameSession::SfxCue cue : m_gameSession.ConsumeSfxEvents())
    {
        m_audioEngine.PlaySfx(cue == GameSession::SfxCue::MultiplierUp ? m_sfxMultiplierUp : m_sfxStreakBroken);
    }
}

// Advances the game session and reflects any judgement in the note lane.
// Once a song completes while it's the visible screen, switches back to the
// song list - playback itself (whatever's still looping) is left running
// untouched, exactly as it already would without this UI on top of it.
void MainWindow::OnTimer(WPARAM timerId)
{
    for (const GamepadInputManager::ButtonEvent& event : m_gamepadInput.Poll())
    {
        OnGamepadButton(event.button, event.pressed);
    }

    m_gameSession.Update();
    DrainJudgements();

    if (m_screen == UiScreen::Playing && m_gameSession.Phase() == GamePhase::Complete)
    {
        HandleSongComplete();
        m_screen = UiScreen::SongSelect;
        ShowWindow(m_hButtonRefresh, SW_SHOW);
        ShowWindow(m_hButtonAssign, SW_SHOW);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    m_noteLane.OnTimer(timerId);
}

// See the header's own comment.
void MainWindow::HandleSongComplete()
{
    int score = m_gameSession.CurrentScore();
    std::wstring songKey = SongKeyForChartPath(m_playingChartPath);

    m_lastResultText =
        L"You scored " + FormatScoreWithCommas(score) + L" on \"" + m_playingSongTitle + L"\"";

    std::vector<HighScoreEntry> entries = m_settings.LoadHighScores(songKey);
    if (!Settings::HighScoreQualifies(entries, score))
    {
        return;
    }

    // No initials prompt - a qualifying run just records itself immediately
    // under a fixed placeholder (never shown anywhere; the high score list's
    // own initials column has no reader left once the prompt that used to
    // display it is gone) and refreshes this song's "Best" label right away.
    Settings::InsertHighScore(entries, L"YOU", score);
    m_settings.SaveHighScores(songKey, entries);
    for (size_t i = 0; i < m_songs.size(); ++i)
    {
        if (m_songs[i].chartPath == m_playingChartPath && i < m_songBestScores.size())
        {
            m_songBestScores[i] = entries.front().score;
            break;
        }
    }
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
    ShowWindow(m_hButtonAssign, SW_SHOW);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Stops the session/animation/audio engine and quits the message loop.
void MainWindow::OnDestroy()
{
    m_gameSession.Stop();
    m_noteLane.StopAnimating();
    m_midiInput.CloseAll();
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
            DrawLastResult(m_backBufferDC);
            DrawEasyModeToggle(m_backBufferDC);
            DrawCapturePrompt(m_backBufferDC);
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

// Custom-paints the refresh and Assign Inputs buttons via DrawGameButton.
void MainWindow::OnDrawItem(LPARAM lParam)
{
    const DRAWITEMSTRUCT& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
    if (item.CtlID == IDC_BUTTON_REFRESH)
    {
        DrawGameButton(item, c_RefreshButtonColor, L"Refresh");
    }
    else if (item.CtlID == IDC_BUTTON_ASSIGN)
    {
        DrawGameButton(item, c_AssignButtonColor, L"Assign Inputs");
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
    m_songs = SongLibrary::Scrape(c_ContentRoot, reportValidationErrors ? &validationErrors : nullptr);

    // Rebuilt alongside m_songs, not read from Settings on every paint - see
    // m_songBestScores's own comment.
    m_songBestScores.assign(m_songs.size(), -1);
    for (size_t i = 0; i < m_songs.size(); ++i)
    {
        std::vector<HighScoreEntry> entries = m_settings.LoadHighScores(SongKeyForChartPath(m_songs[i].chartPath));
        if (!entries.empty())
        {
            m_songBestScores[i] = entries.front().score;
        }
    }

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
        MessageBoxW(m_hwnd, message.c_str(), c_WindowTitle, MB_OK | MB_ICONWARNING);
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
        MessageBoxW(m_hwnd, message.c_str(), c_WindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    m_settings.SaveLastChartPath(m_songs[index].chartPath);
    m_playingChartPath = m_songs[index].chartPath;
    m_playingSongTitle = m_songs[index].title;
    m_lastResultText.clear();
    m_gameSession.Start();
    m_screen = UiScreen::Playing;
    ShowWindow(m_hButtonRefresh, SW_HIDE);
    ShowWindow(m_hButtonAssign, SW_HIDE);
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

    RECT headerRect{c_RowLeft, c_RowTop, c_RowLeft + 260, c_RowTop + c_ControlHeight + 6};
    HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
    SetTextColor(hdc, c_LabelTextColor);
    DrawTextW(hdc, L"Choose a Song", -1, &headerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, rowFont);

    if (m_songs.empty())
    {
        SetTextColor(hdc, c_LabelTextColor);
        RECT emptyRect = rect;
        DrawTextW(hdc, L"No songs found in Content\\ - add a song folder, then press Refresh.", -1, &emptyRect,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        return;
    }

    for (size_t i = 0; i < m_songs.size(); ++i)
    {
        RECT rowRect{rect.left, rect.top + static_cast<int>(i) * c_SongRowHeight, rect.right,
                     rect.top + static_cast<int>(i + 1) * c_SongRowHeight};
        if (rowRect.top >= rect.bottom)
        {
            break;
        }

        bool selected = (static_cast<int>(i) == m_selectedSongIndex);
        if (selected)
        {
            HBRUSH highlightBrush = CreateSolidBrush(c_SongRowHighlightColor);
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
        SetTextColor(hdc, selected ? c_SongRowHighlightTextColor : c_LabelTextColor);

        if (i < m_songBestScores.size() && m_songBestScores[i] >= 0)
        {
            RECT scoreRect = textRect;
            scoreRect.left = std::max(scoreRect.left, scoreRect.right - 150);
            std::wstring scoreText = L"Best " + FormatScoreWithCommas(m_songBestScores[i]);
            DrawTextW(hdc, scoreText.c_str(), -1, &scoreRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            textRect.right = scoreRect.left - 8;
        }

        DrawTextW(hdc, m_songs[i].title.c_str(), -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    RECT hintRect{rect.left, rect.bottom + 10, rect.right, rect.bottom + 34};
    SelectObject(hdc, hintFont);
    SetTextColor(hdc, c_HintTextColor);
    DrawTextW(hdc, L"Click a song, or use \x2191/\x2193 and any other key to start it.", -1, &hintRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, oldFont);
}

// See the header's own comment. Drawn in the gap between the header row and
// the song list, so it never competes with either for space. Suppressed
// while the capture-assignment overlay prompt is up (m_captureLane != -1) -
// it spills downward from the same header-row starting point (see
// DrawCapturePrompt) and would otherwise land right on top of this text.
void MainWindow::DrawLastResult(HDC hdc)
{
    if (m_lastResultText.empty() || m_captureLane != -1)
    {
        return;
    }

    static HFONT resultFont =
        CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    RECT rect{c_RowLeft, c_ToolbarHeight, m_songListRect.right, c_ToolbarHeight + c_LaneMargin};
    HFONT oldFont = (HFONT)SelectObject(hdc, resultFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c_SongRowHighlightColor);
    DrawTextW(hdc, m_lastResultText.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
    RECT trackRect{rect.right - c_ToggleTrackWidth, centerY - c_ToggleTrackHeight / 2, rect.right,
                    centerY + c_ToggleTrackHeight / 2};

    HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));

    HBRUSH trackBrush = CreateSolidBrush(m_easyMode ? c_ToggleTrackOnColor : c_ToggleTrackOffColor);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, trackBrush);
    RoundRect(hdc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom, c_ToggleTrackHeight,
              c_ToggleTrackHeight);
    SelectObject(hdc, oldBrush);
    DeleteObject(trackBrush);

    int knobCenterX = m_easyMode ? trackRect.right - c_ToggleTrackHeight / 2 : trackRect.left + c_ToggleTrackHeight / 2;
    HBRUSH knobBrush = CreateSolidBrush(c_ToggleKnobColor);
    SelectObject(hdc, knobBrush);
    Ellipse(hdc, knobCenterX - c_ToggleKnobRadius, centerY - c_ToggleKnobRadius, knobCenterX + c_ToggleKnobRadius,
            centerY + c_ToggleKnobRadius);
    SelectObject(hdc, oldBrush);
    DeleteObject(knobBrush);

    SelectObject(hdc, oldPen);

    RECT labelRect{rect.left, rect.top, trackRect.left - 8, rect.bottom};
    HFONT oldFont = (HFONT)SelectObject(hdc, labelFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c_LabelTextColor);
    DrawTextW(hdc, L"Easy Mode", -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

// See the header's own comment.
void MainWindow::DrawCapturePrompt(HDC hdc)
{
    if (m_captureLane == -1)
    {
        return;
    }

    static HFONT promptFont =
        CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    std::wstring text = L"Press a key, MIDI note, or gamepad button for lane " + std::to_wstring(m_captureLane + 1) +
                         L" of " + std::to_wstring(c_LaneCount) + L" - Esc to stop";
    if (!m_captureRejectionMessage.empty())
    {
        text += L"\r\n" + m_captureRejectionMessage;
    }

    // Deliberately tall enough to spill over the song list's own top rows -
    // input is fully blocked while capturing (OnLButtonDown/OnKeyDown both
    // guard on m_captureLane), so overlapping them briefly is harmless and
    // keeps this from needing its own dedicated layout real estate.
    int promptTop = c_RowTop + c_ControlHeight + 10;
    RECT rect{c_RowLeft, promptTop, m_songListRect.right, promptTop + 60};
    HFONT oldFont = (HFONT)SelectObject(hdc, promptFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c_SongRowHighlightColor);
    DrawTextW(hdc, text.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
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

    int row = (pt.y - m_songListRect.top) / c_SongRowHeight;
    if (row < 0 || row >= static_cast<int>(m_songs.size()))
    {
        return -1;
    }
    return row;
}
