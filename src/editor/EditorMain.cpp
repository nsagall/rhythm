// wWinMain entry point for RhythmEditor.exe: registers the window class, creates a DX11
// device/swapchain for ImGui's DX11 backend, wires up the Win32+DX11 ImGui backends, and runs the
// message loop, handing every frame to EditorApp::Update().
#include <d3d11.h>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "EditorApp.h"
#include "EditorSettings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
EditorApp* g_app = nullptr;
bool g_destroyRequested = false;

// Separate from EditorApp's EditorSettings member (EditorSettings is stateless), because window
// placement must be loaded before EditorApp is constructed and saved from WndProc.
EditorSettings g_windowSettings;
// True from WM_ENTERSIZEMOVE to WM_EXITSIZEMOVE. WM_SIZE fires once per pixel during a drag, so
// gating the maximize/restore save on !g_inSizeMove avoids writing the ini dozens of times a
// second; WM_EXITSIZEMOVE saves once when the drag ends.
bool g_inSizeMove = false;

void SaveCurrentWindowPlacement(HWND hwnd)
{
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(WINDOWPLACEMENT);
    if (GetWindowPlacement(hwnd, &placement))
    {
        g_windowSettings.SaveWindowPlacement(placement);
    }
}

// A saved placement can be stale (an unplugged monitor, a resolution change) - reject it rather
// than create a window the user can't reach, and fall back to the default.
bool IsPlacementOnScreen(const RECT& rect)
{
    RECT virtualScreen;
    virtualScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtualScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtualScreen.right = virtualScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtualScreen.bottom = virtualScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    RECT intersection;
    return IntersectRect(&intersection, &rect, &virtualScreen) != 0 && (rect.right - rect.left) >= 200 &&
           (rect.bottom - rect.top) >= 150;
}

void CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer != nullptr)
    {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView != nullptr)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
    {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(hr))
    {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain != nullptr)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext != nullptr)
    {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice != nullptr)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    {
        return true;
    }

    switch (msg)
    {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
                                             DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            // Only maximize/restore here - not every intermediate drag size (WM_EXITSIZEMOVE covers
            // that), and never while minimized.
            if (!g_inSizeMove && (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED))
            {
                SaveCurrentWindowPlacement(hWnd);
            }
            return 0;
        case WM_ENTERSIZEMOVE:
            g_inSizeMove = true;
            break;
        case WM_EXITSIZEMOVE:
            g_inSizeMove = false;
            SaveCurrentWindowPlacement(hWnd);
            break;
        case WM_CLOSE:
            if (g_app != nullptr)
            {
                g_app->RequestQuit();
                if (!g_app->WantsToQuit())
                {
                    // Dirty document - the unsaved-changes modal is showing; stay open until it's
                    // resolved (the main loop's WantsToQuit() poll closes the window then).
                    return 0;
                }
            }
            break;
        case WM_DESTROY:
            // Final safety net - covers an exit that never went through an interactive
            // move/resize, so WM_EXITSIZEMOVE never fired.
            SaveCurrentWindowPlacement(hWnd);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RhythmEditorWindowClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RhythmEditor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1440, 900, nullptr, nullptr, hInstance, nullptr);

    // Restore the last session's placement (if on-screen) before CreateDeviceD3D reads the client
    // rect for the swap chain - applying it later would show a default-sized window for one frame,
    // then jump.
    WINDOWPLACEMENT savedPlacement = {};
    bool hasSavedPlacement =
        g_windowSettings.LoadWindowPlacement(savedPlacement) && IsPlacementOnScreen(savedPlacement.rcNormalPosition);
    if (hasSavedPlacement)
    {
        WINDOWPLACEMENT hiddenPlacement = savedPlacement;
        hiddenPlacement.showCmd = SW_HIDE; // stay invisible until the normal ShowWindow below - avoids a flash
        SetWindowPlacement(hwnd, &hiddenPlacement);
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, hasSavedPlacement ? savedPlacement.showCmd : nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    EditorApp app;
    g_app = &app;
    app.Initialize(hwnd);

    const ImVec4 clearColor = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                done = true;
            }
        }
        if (done)
        {
            break;
        }

        // While minimized, GetClientRect reports a tiny placeholder rect. A frame with that as
        // io.DisplaySize would have EditorApp::Update()'s pane-splitter clamping permanently shrink
        // every persisted pane size. Skip the frame entirely (there's nothing to draw anyway); the
        // sleep keeps this from busy-spinning.
        if (IsIconic(hwnd))
        {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Update();

        if (app.WantsToQuit() && !g_destroyRequested)
        {
            g_destroyRequested = true;
            DestroyWindow(hwnd);
        }

        ImGui::Render();
        const float clearColorArray[4] = {clearColor.x * clearColor.w, clearColor.y * clearColor.w,
                                           clearColor.z * clearColor.w, clearColor.w};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorArray);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    app.Shutdown();
    g_app = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
