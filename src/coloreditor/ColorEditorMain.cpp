// wWinMain entry point for ColorEditor.exe: a small standalone tool for
// tuning Colors.ini (see ColorConfig.h) without touching game code. Sets
// up the same Win32 + DirectX11 + Dear ImGui plumbing as RhythmEditor
// (EditorMain.cpp), trimmed down since this tool has no document to load,
// no window-placement persistence, and nothing that can be "dirty" beyond
// the color values themselves.
#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "ColorConfig.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

constexpr wchar_t kWindowClassName[] = L"ColorEditorWindowClass";

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
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Converts a live COLORREF into the 0-1 float triple ImGui's color
// widgets want, edits it in place via a hue-wheel picker popup, and
// writes any change straight back through valuePtr - the same global
// Colors::SaveToIni will read from when the user hits Save. Rendered as a
// bare swatch (ImGuiColorEditFlags_NoInputs) with the entry's own name
// underneath, rather than inline R/G/B sliders, so a whole palette reads
// as a labeled grid instead of a tall list of number fields.
bool DrawSwatch(const char* name, COLORREF* valuePtr)
{
    float color[3] = {
        GetRValue(*valuePtr) / 255.0f,
        GetGValue(*valuePtr) / 255.0f,
        GetBValue(*valuePtr) / 255.0f,
    };

    ImGui::PushID(name);
    ImGui::BeginGroup();
    bool changed = ImGui::ColorEdit3(
        "##swatch", color,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoLabel);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 96.0f);
    ImGui::TextUnformatted(name);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopID();

    if (changed)
    {
        auto toByte = [](float c) { return static_cast<int>(c * 255.0f + 0.5f); };
        *valuePtr = RGB(toByte(color[0]), toByte(color[1]), toByte(color[2]));
    }
    return changed;
}

// Lays every entry out as a wrapping grid of DrawSwatch()es, grouped
// under a header per Colors::Entry::section - the standard ImGui
// "wrap onto the next line once the next item would overflow the window"
// idiom (see imgui_demo.cpp's own Layout > Text Wrapping /
// Basic Helpers > Button Wrapping examples), keyed off each swatch
// group's own measured width rather than a guessed fixed size.
bool DrawColorGrid()
{
    const std::vector<Colors::Entry>& entries = Colors::AllEntries();
    const ImGuiStyle& style = ImGui::GetStyle();
    float windowVisibleRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    bool anyChanged = false;
    const char* currentSection = "";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Colors::Entry& entry = entries[i];
        bool sectionChanged = std::strcmp(entry.section, currentSection) != 0;
        if (sectionChanged)
        {
            currentSection = entry.section;
            ImGui::SeparatorText(currentSection);
        }

        anyChanged |= DrawSwatch(entry.name, entry.value);

        bool nextIsSameSection = (i + 1 < entries.size()) && std::strcmp(entries[i + 1].section, currentSection) == 0;
        float groupRight = ImGui::GetItemRectMax().x;
        float nextGroupRight = groupRight + style.ItemSpacing.x + ImGui::GetItemRectSize().x;
        if (nextIsSameSection && nextGroupRight < windowVisibleRight)
        {
            ImGui::SameLine();
        }
    }
    return anyChanged;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Color Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1000, 760, nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Colors::LoadFromIni();
    bool dirty = false;

    const ImVec4 clearColor = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);

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

        if (IsIconic(hwnd))
        {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("ColorEditorRoot", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (ImGui::Button("Save to Colors.ini"))
        {
            Colors::SaveToIni();
            dirty = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload from disk"))
        {
            Colors::LoadFromIni();
            dirty = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(dirty ? "Unsaved changes" : "All changes saved");

        ImGui::Separator();

        ImGui::BeginChild("ColorGrid");
        dirty |= DrawColorGrid();
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clearColorArray[4] = {clearColor.x * clearColor.w, clearColor.y * clearColor.w,
                                           clearColor.z * clearColor.w, clearColor.w};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorArray);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
