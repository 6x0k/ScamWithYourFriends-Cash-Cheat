#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <tchar.h>
#include <string>
#include <algorithm>
#include <vector>
#include <tlhelp32.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

DWORD g_pid = 0;
HANDLE g_process = nullptr;
uintptr_t g_monoBase = 0;

const uintptr_t BASE_OFFSET = 0x00A016B0;
const std::vector<uintptr_t> OFFSETS = { 0x141, 0x20, 0x8D, 0x18, 0x10, 0x60, 0xF8 };

DWORD GetProcessIdByName(const std::wstring& processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                if (processName == processEntry.szExeFile) { pid = processEntry.th32ProcessID; break; }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

uintptr_t GetModuleBaseAddress(DWORD pid, const std::wstring& moduleName) {
    uintptr_t baseAddress = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W moduleEntry;
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (Module32FirstW(snapshot, &moduleEntry)) {
            do {
                if (moduleName == moduleEntry.szModule) { baseAddress = (uintptr_t)moduleEntry.modBaseAddr; break; }
            } while (Module32NextW(snapshot, &moduleEntry));
        }
        CloseHandle(snapshot);
    }
    return baseAddress;
}

uintptr_t ResolvePointerChain(uintptr_t baseAddress, const std::vector<uintptr_t>& offsets) {
    uintptr_t currentAddress = baseAddress;
    for (size_t i = 0; i < offsets.size(); i++) {
        uintptr_t pointerValue = 0;
        if (!ReadProcessMemory(g_process, (LPCVOID)currentAddress, &pointerValue, sizeof(pointerValue), nullptr)) return 0;
        currentAddress = pointerValue + offsets[i];
    }
    return currentAddress;
}

static HWND g_overlay = nullptr;
static HWND g_game = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_menuVisible = true;
static bool g_running = true;
static bool g_insertWasDown = false;

static void CleanupRenderTarget() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}
static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &obtained, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return g_rtv != nullptr;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}
static HWND FindGameWindow() {
    HWND found = FindWindowW(nullptr, L"Scam With Your Friends");
    if (found) return found;
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[512]{}; GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        if (wcsstr(title, L"Scam With Your Friends")) { *reinterpret_cast<HWND*>(param) = hwnd; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
static void SetMenuVisibility(bool visible) {
    g_menuVisible = visible;
    LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
    if (visible) { ex &= ~WS_EX_TRANSPARENT; SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex); SetForegroundWindow(g_overlay); }
    else { ex |= WS_EX_TRANSPARENT; SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex); }
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY: g_running = false; PostQuitMessage(0); return 0;
    case WM_NCHITTEST: if (!g_menuVisible) return HTTRANSPARENT; break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_game = FindGameWindow();
    if (!g_game) { MessageBoxW(nullptr, L"Could not find the game window.\nStart the game first.", L"Error", MB_ICONWARNING | MB_OK); return 1; }

    g_pid = GetProcessIdByName(L"Scam With Your Friends.exe");
    if (g_pid == 0) { MessageBoxW(nullptr, L"Could not find process ID. Are you running as Administrator?", L"Error", MB_ICONERROR); return 1; }

    g_process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
    if (!g_process) {
        DWORD err = GetLastError();
        wchar_t errMsg[256];
        swprintf_s(errMsg, L"Failed to open process. Error Code: %lu\nMake sure you are running as Administrator.", err);
        MessageBoxW(nullptr, errMsg, L"Error", MB_ICONERROR);
        return 1;
    }

    Sleep(100);
    g_monoBase = GetModuleBaseAddress(g_pid, L"mono-2.0-bdwgc.dll");
    if (g_monoBase == 0) { MessageBoxW(nullptr, L"Could not find mono-2.0-bdwgc.dll.\nMake sure you are actually in a match.", L"Error", MB_ICONERROR); return 1; }

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"SWYFOverlayWindow", nullptr };
    RegisterClassExW(&wc);
    RECT gameRect{}; GetWindowRect(g_game, &gameRect);
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, wc.lpszClassName, L"SWYF Overlay",
        WS_POPUP, gameRect.left, gameRect.top, gameRect.right - gameRect.left, gameRect.bottom - gameRect.top,
        nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    if (!CreateDeviceD3D(g_overlay)) { CleanupDeviceD3D(); DestroyWindow(g_overlay); UnregisterClassW(wc.lpszClassName, instance); return 1; }

    ShowWindow(g_overlay, SW_SHOW); UpdateWindow(g_overlay);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark(); ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f; style.FrameRounding = 6.0f; style.GrabRounding = 6.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.075f, 0.97f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.35f, 0.18f, 0.65f, 0.85f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.32f, 0.16f, 0.58f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.43f, 0.24f, 0.74f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.55f, 1.0f, 1.0f);

    ImGui_ImplWin32_Init(g_overlay);
    ImGui_ImplDX11_Init(g_device, g_context);
    SetMenuVisibility(true);

    char moneyText[32] = "1000000";
    long long currentCash = 0;
    long long targetCash = 0;
    bool freezeMoney = false;
    uintptr_t finalAddress = 0;

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertDown && !g_insertWasDown) SetMenuVisibility(!g_menuVisible);
        g_insertWasDown = insertDown;

        if (!IsWindow(g_game)) { g_game = FindGameWindow(); if (!g_game) { Sleep(100); continue; } }
        if (IsIconic(g_game)) { ShowWindow(g_overlay, SW_HIDE); Sleep(30); continue; }
        ShowWindow(g_overlay, SW_SHOW);
        GetWindowRect(g_game, &gameRect);
        SetWindowPos(g_overlay, HWND_TOPMOST, gameRect.left, gameRect.top, gameRect.right - gameRect.left, gameRect.bottom - gameRect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();

        if (g_process && g_monoBase != 0) {
            finalAddress = ResolvePointerChain(g_monoBase + BASE_OFFSET, OFFSETS);
            if (finalAddress != 0) {
                ReadProcessMemory(g_process, (LPCVOID)finalAddress, &currentCash, sizeof(currentCash), nullptr);
                if (freezeMoney) WriteProcessMemory(g_process, (LPVOID)finalAddress, &targetCash, sizeof(targetCash), nullptr);
            }
        }

        if (g_menuVisible) {
            ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(390, 320), ImGuiCond_Once);
            ImGui::Begin("SWYF | Trainer", nullptr, ImGuiWindowFlags_NoCollapse);
            
            ImGui::TextColored(ImVec4(0.75f, 0.58f, 1.0f, 1.0f), "SCAM WITH YOUR FRIENDS");
            ImGui::TextDisabled("Memory Trainer v1.2");
            ImGui::Separator();
            
            if (finalAddress != 0) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Status: Attached");
                ImGui::Text("Pointer Addr: 0x%llX", (unsigned long long)finalAddress);
                ImGui::Text("Current Cash: %lld", currentCash);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Status: Pointer chain broken");
            }
            
            ImGui::Spacing();
            ImGui::InputText("New Cash Value", moneyText, sizeof(moneyText));
            
            if (ImGui::Button(freezeMoney ? "Stop Freeze" : "Set & Freeze Money", ImVec2(-1, 32))) {
                if (finalAddress != 0 && g_process) {
                    freezeMoney = !freezeMoney;
                    if (freezeMoney) {
                        targetCash = std::stoll(moneyText);
                        WriteProcessMemory(g_process, (LPVOID)finalAddress, &targetCash, sizeof(targetCash), nullptr);
                    }
                } else {
                    MessageBoxW(g_overlay, L"Cannot write memory. Address not found.", L"Error", MB_OK | MB_ICONERROR);
                }
            }
            
            ImGui::Spacing();
            ImGui::TextDisabled("Press INSERT to hide/show. END closes.");
            ImGui::End();
        }

        ImGui::Render();
        const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);

        if (GetAsyncKeyState(VK_END) & 1) g_running = false;
    }

    if (g_process) CloseHandle(g_process);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); DestroyWindow(g_overlay); UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}