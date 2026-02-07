#define _WIN32_WINNT 0x0A00 // Windows 10
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <mmsystem.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm> // For std::find

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")
#pragma comment (lib, "shell32.lib")
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxgi.lib")
#pragma comment (lib, "d2d1.lib")
#pragma comment (lib, "dcomp.lib")
#pragma comment (lib, "winmm.lib")

// Safe Release helper
template<class Interface>
inline void SafeRelease(Interface** ppInterfaceToRelease) {
    if (*ppInterfaceToRelease != NULL) {
        (*ppInterfaceToRelease)->Release();
        (*ppInterfaceToRelease) = NULL;
    }
}

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_TRAY_RELOAD 9002
#define ID_TOGGLE_BORDER 9006 // ID for Border Toggle
#define ID_EDIT_CONFIG 9007
#define ID_OPEN_LOCATION 9008
#define ID_PRESET_BASE 10000 // Base ID for Dynamic Presets

// Global variables
NOTIFYICONDATA nid = { 0 };
HICON hAppIcon = NULL; // Global Icon Handle
std::vector<std::string> g_PresetNames; // Dynamic Preset List

// DirectX / DComp Objects
ID3D11Device* pD3DDevice = nullptr;
IDXGIDevice* pDXGIDevice = nullptr;
ID2D1Factory1* pD2DFactory = nullptr;
ID2D1Device* pD2DDevice = nullptr;
ID2D1DeviceContext* pD2DContext = nullptr;
IDXGISwapChain1* pSwapChain = nullptr;
IDCompositionDevice* pDCompDevice = nullptr;
IDCompositionTarget* pDCompTarget = nullptr;
IDCompositionVisual* pDCompVisual = nullptr;

// 1. Config Structure
struct Timing {
    float inhale = 4.0f;
    float holdIn = 4.0f;
    float exhale = 4.0f;
    float holdEx = 4.0f;
};

struct Visuals {
    float minRadius = 20.0f;
    float maxRadius = 380.0f;
    int r = 26;
    int g = 115;
    int b = 232;
    int alpha = 100;
    bool showBorder = true;
};

struct AppConfig {
    char activePreset[64] = "Normal";
    Timing currentTiming;
    Visuals visuals;
} g_Config;

// Animation State
enum BreathingState { INHALE, HOLD_IN, EXHALE, HOLD_EX };
BreathingState g_State = INHALE;
float g_StateTimer = 0.0f;
LARGE_INTEGER g_LastFrameTime;
LARGE_INTEGER g_Frequency;

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void LoadConfig();
void CreateDefaultConfigIfMissing(const char* path);
void SavePreset(const char* presetName);
void SaveSetting(const char* key, int value);
bool InitDirectX(HWND hwnd);
void Render();
void CleanupDirectX();
void ResetAnimation();
float GetIniFloat(const char* section, const char* key, float defaultValue, const char* path);
HICON CreateProceduralIcon(int size);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    // Generate Soft Burst Icon (32x32)
    hAppIcon = CreateProceduralIcon(32);

    // Initialize Timer
    QueryPerformanceFrequency(&g_Frequency);
    QueryPerformanceCounter(&g_LastFrameTime);

    LoadConfig();

    const char* className = "BreathingOverlayClass";
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = className;
    // Set Class Icon
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIcon;

    RegisterClassEx(&wc);

    int width = 800;
    int height = 800;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int xPos = (screenWidth - width) / 2;
    int yPos = (screenHeight - height) / 2;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        className,
        "Breathing",
        WS_POPUP | WS_VISIBLE,
        xPos, yPos, width, height,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    if (!InitDirectX(hwnd)) {
        CleanupDirectX();
        return 0;
    }

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1001;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // Set Tray Icon
    nid.hIcon = hAppIcon;
    lstrcpy(nid.szTip, "Breathing Overlay");
    Shell_NotifyIcon(NIM_ADD, &nid);

    MSG msg = { 0 };
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            Render();
        }
    }

    timeEndPeriod(1);
    CleanupDirectX();
    return (int)msg.wParam;
}

// 1. Procedural Icon Generator (Raw C++ Math)
HICON CreateProceduralIcon(int size) {
    // 32-bit RGBA (4 bytes per pixel)
    std::vector<uint32_t> pixels(size * size);

    float centerX = size / 2.0f;
    float centerY = size / 2.0f;
    float maxRadius = (size / 2.0f) - 1.0f;
    float amplitude = maxRadius * 0.20f;
    float baseRadius = maxRadius - amplitude;

    // Iterate pixels
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy);
            float theta = atan2f(dy, dx);

            // Soft Burst Shape: r = base + amp * cos(8 * theta)
            float burstRadius = baseRadius + amplitude * cosf(8.0f * theta);

            // Anti-aliasing / Soft Edge
            // If dist < burstRadius, alpha = 255.
            // If dist > burstRadius + 1, alpha = 0.
            // In between, interpolate.
            float edgeDist = dist - burstRadius;
            float alphaFactor = 0.0f;

            if (edgeDist < -0.5f) alphaFactor = 1.0f;
            else if (edgeDist > 0.5f) alphaFactor = 0.0f;
            else alphaFactor = 0.5f - edgeDist; // Linear fade

            uint8_t a = (uint8_t)(alphaFactor * 255.0f);
            uint8_t r = 26;
            uint8_t g = 115;
            uint8_t b = 232;

            // Pre-multiplied Alpha is often safer, but for CreateIcon cursor mask logic, raw values usually work.
            // Windows Icon format: BGRA
            // We'll write 0xAARRGGBB

            if (a > 0) {
                pixels[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            } else {
                pixels[y * size + x] = 0;
            }
        }
    }

    // Create Bitmaps for the Icon
    HBITMAP hColorObj = CreateBitmap(size, size, 1, 32, pixels.data());

    // Mask is required
    std::vector<uint8_t> maskPixels((size * size) / 8, 0xFF); // All 1s
    HBITMAP hMaskObj = CreateBitmap(size, size, 1, 1, maskPixels.data());

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hMaskObj;
    ii.hbmColor = hColorObj;

    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hColorObj);
    DeleteObject(hMaskObj);

    return hIcon;
}

bool InitDirectX(HWND hwnd) {
    HRESULT hr;

    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &pD3DDevice, nullptr, nullptr
    );
    if (FAILED(hr)) return false;

    hr = pD3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    if (FAILED(hr)) return false;

    D2D1_FACTORY_OPTIONS options = { D2D1_DEBUG_LEVEL_NONE };
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, (void**)&pD2DFactory);
    if (FAILED(hr)) return false;

    hr = pD2DFactory->CreateDevice(pDXGIDevice, &pD2DDevice);
    if (FAILED(hr)) return false;

    hr = pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pD2DContext);
    if (FAILED(hr)) return false;

    IDXGIAdapter* pAdapter = nullptr;
    pDXGIDevice->GetAdapter(&pAdapter);
    IDXGIFactory2* pDXGIFactory = nullptr;
    pAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pDXGIFactory);

    DXGI_SWAP_CHAIN_DESC1 scDesc = { 0 };
    scDesc.Width = 800;
    scDesc.Height = 800;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = pDXGIFactory->CreateSwapChainForComposition(pD3DDevice, &scDesc, nullptr, &pSwapChain);
    SafeRelease(&pAdapter);
    SafeRelease(&pDXGIFactory);
    if (FAILED(hr)) return false;

    IDXGISurface* pSurface = nullptr;
    hr = pSwapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&pSurface);
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    ID2D1Bitmap1* pBitmap = nullptr;
    hr = pD2DContext->CreateBitmapFromDxgiSurface(pSurface, &bp, &pBitmap);
    pD2DContext->SetTarget(pBitmap);

    SafeRelease(&pSurface);
    SafeRelease(&pBitmap);
    if (FAILED(hr)) return false;

    hr = DCompositionCreateDevice(pDXGIDevice, __uuidof(IDCompositionDevice), (void**)&pDCompDevice);
    if (FAILED(hr)) return false;

    hr = pDCompDevice->CreateTargetForHwnd(hwnd, TRUE, &pDCompTarget);
    if (FAILED(hr)) return false;

    hr = pDCompDevice->CreateVisual(&pDCompVisual);
    if (FAILED(hr)) return false;

    hr = pDCompVisual->SetContent(pSwapChain);
    if (FAILED(hr)) return false;

    hr = pDCompTarget->SetRoot(pDCompVisual);
    if (FAILED(hr)) return false;

    hr = pDCompDevice->Commit();
    if (FAILED(hr)) return false;

    return true;
}

// Helper to read floats from INI
float GetIniFloat(const char* section, const char* key, float defaultValue, const char* path) {
    char defaultStr[32];
    sprintf_s(defaultStr, "%.2f", defaultValue);
    char buffer[64];
    GetPrivateProfileStringA(section, key, defaultStr, buffer, 64, path);
    return (float)atof(buffer);
}

void CreateDefaultConfigIfMissing(const char* path) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* pFile;
        // fopen_s returns 0 on success
        if (fopen_s(&pFile, path, "w") == 0) {
            fputs("[Settings]\n"
                  "ActivePreset=Normal\n"
                  "MaxRadius=320\n"
                  "MinRadius=60\n"
                  "ShowBorder=1\n"
                  "\n"
                  "Alpha=50\n"
                  "ColorR=26\n"
                  "ColorG=115\n"
                  "ColorB=232\n"
                  "\n\n"
                  "[Normal]\n"
                  "Inhale=4.0\n"
                  "HoldIn=1.0\n"
                  "Exhale=4.0\n"
                  "HoldEx=1.0\n"
                  "\n"
                  "[Focus]\n"
                  "Inhale=4.0\n"
                  "HoldIn=7.0\n"
                  "Exhale=8.0\n"
                  "HoldEx=0.0\n"
                  "\n"
                  "[Quick]\n"
                  "Inhale=2.0\n"
                  "HoldIn=0.0\n"
                  "Exhale=2.0\n"
                  "HoldEx=0.0\n", pFile);
            fclose(pFile);
        }
    }
}

void LoadConfig() {
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    lstrcatA(path, "Breathing-config.ini"); // Ensure consistent filename

    // Auto-create defaults if missing
    CreateDefaultConfigIfMissing(path);

    // Dynamic Preset Loading
    g_PresetNames.clear();
    char buffer[2048]; // Large buffer for section names
    if (GetPrivateProfileSectionNamesA(buffer, 2048, path) > 0) {
        char* p = buffer;
        while (*p) {
            std::string section(p);
            if (section != "Settings") {
                g_PresetNames.push_back(section);
            }
            p += strlen(p) + 1;
        }
    }

    // Read Active Preset (Temporary)
    char tempActive[64];
    GetPrivateProfileStringA("Settings", "ActivePreset", "Normal", tempActive, 64, path);

    // --- SELF HEALING LOGIC ---
    std::string validatedPreset = tempActive;
    bool found = false;

    for (const auto& name : g_PresetNames) {
        if (name == validatedPreset) {
            found = true;
            break;
        }
    }

    if (!found) {
        if (!g_PresetNames.empty()) {
            // Fallback to the first available preset
            validatedPreset = g_PresetNames[0];
            // CRITICAL: Write correction back to INI
            WritePrivateProfileStringA("Settings", "ActivePreset", validatedPreset.c_str(), path);
        } else {
            // No presets found at all? Should not happen due to CreateDefaultConfigIfMissing
            validatedPreset = "Normal";
        }
    }

    // Set the validated preset as active
    strcpy_s(g_Config.activePreset, validatedPreset.c_str());
    // ---------------------------

    // Read Timings from the Active Preset Section
    g_Config.currentTiming.inhale = GetIniFloat(g_Config.activePreset, "Inhale", 4.0f, path);
    g_Config.currentTiming.holdIn = GetIniFloat(g_Config.activePreset, "HoldIn", 4.0f, path);
    g_Config.currentTiming.exhale = GetIniFloat(g_Config.activePreset, "Exhale", 4.0f, path);
    g_Config.currentTiming.holdEx = GetIniFloat(g_Config.activePreset, "HoldEx", 4.0f, path);

    // Read Visuals from Settings
    g_Config.visuals.minRadius = GetIniFloat("Settings", "MinRadius", 20.0f, path);
    g_Config.visuals.maxRadius = GetIniFloat("Settings", "MaxRadius", 380.0f, path);
    g_Config.visuals.r = GetPrivateProfileIntA("Settings", "ColorR", 26, path);
    g_Config.visuals.g = GetPrivateProfileIntA("Settings", "ColorG", 115, path);
    g_Config.visuals.b = GetPrivateProfileIntA("Settings", "ColorB", 232, path);
    g_Config.visuals.alpha = GetPrivateProfileIntA("Settings", "Alpha", 100, path);
    g_Config.visuals.showBorder = GetPrivateProfileIntA("Settings", "ShowBorder", 1, path) != 0;
}

void SavePreset(const char* presetName) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    lstrcatA(path, "Breathing-config.ini");

    WritePrivateProfileStringA("Settings", "ActivePreset", presetName, path);
}

// Added Save Helper
void SaveSetting(const char* key, int value) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    lstrcatA(path, "Breathing-config.ini");

    char buffer[32];
    sprintf_s(buffer, "%d", value);
    WritePrivateProfileStringA("Settings", key, buffer, path);
}

void ResetAnimation() {
    g_State = INHALE;
    g_StateTimer = 0.0f;
}

void Render() {
    if (!pD2DContext) return;

    // Time Calc
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    double deltaTime = (double)(currentTime.QuadPart - g_LastFrameTime.QuadPart) / g_Frequency.QuadPart;
    g_LastFrameTime = currentTime;

    // Direct2D Start
    pD2DContext->BeginDraw();
    pD2DContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // --- 3. Animation State Machine ---
    g_StateTimer += (float)deltaTime;

    float currentRadius = g_Config.visuals.minRadius;
    float minR = g_Config.visuals.minRadius;
    float maxR = g_Config.visuals.maxRadius;

    if (g_State == INHALE) {
        if (g_StateTimer >= g_Config.currentTiming.inhale) {
            g_State = HOLD_IN;
            g_StateTimer = 0.0f; // Exact Reset
            currentRadius = maxR;
        } else {
            float t = g_StateTimer / g_Config.currentTiming.inhale;
            currentRadius = minR + (maxR - minR) * t; // Linear Lerp
        }
    }
    else if (g_State == HOLD_IN) {
        currentRadius = maxR;
        if (g_StateTimer >= g_Config.currentTiming.holdIn) {
            g_State = EXHALE;
            g_StateTimer = 0.0f; // Exact Reset
        }
    }
    else if (g_State == EXHALE) {
        if (g_StateTimer >= g_Config.currentTiming.exhale) {
            g_State = HOLD_EX;
            g_StateTimer = 0.0f; // Exact Reset
            currentRadius = minR;
        } else {
            float t = g_StateTimer / g_Config.currentTiming.exhale;
            currentRadius = maxR - (maxR - minR) * t; // Linear Lerp
        }
    }
    else if (g_State == HOLD_EX) {
        currentRadius = minR;
        if (g_StateTimer >= g_Config.currentTiming.holdEx) {
            g_State = INHALE;
            g_StateTimer = 0.0f; // Exact Reset
        }
    }
    // --------------------------------

    float centerX = 400.0f;
    float centerY = 400.0f;

    // Draw
    float r = g_Config.visuals.r / 255.0f;
    float g = g_Config.visuals.g / 255.0f;
    float b = g_Config.visuals.b / 255.0f;
    float a = g_Config.visuals.alpha / 255.0f;

    ID2D1SolidColorBrush* pFillBrush = nullptr;
    pD2DContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &pFillBrush);

    if (pFillBrush) {
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), currentRadius, currentRadius);
        pD2DContext->FillEllipse(ellipse, pFillBrush);
        SafeRelease(&pFillBrush);
    }

    if (g_Config.visuals.showBorder) {
        ID2D1SolidColorBrush* pBorderBrush = nullptr;
        pD2DContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 40.0f / 255.0f), &pBorderBrush);
        if (pBorderBrush) {
            D2D1_ELLIPSE borderEllipse = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), maxR, maxR);
            pD2DContext->DrawEllipse(borderEllipse, pBorderBrush, 2.0f);
            SafeRelease(&pBorderBrush);
        }
    }

    HRESULT hr = pD2DContext->EndDraw();

    if (pSwapChain) {
        pSwapChain->Present(1, 0);
    }
}

void CleanupDirectX() {
    if (hAppIcon) DestroyIcon(hAppIcon);
    SafeRelease(&pDCompVisual);
    SafeRelease(&pDCompTarget);
    SafeRelease(&pDCompDevice);
    SafeRelease(&pD2DContext);
    SafeRelease(&pD2DDevice);
    SafeRelease(&pD2DFactory);
    SafeRelease(&pSwapChain);
    SafeRelease(&pDXGIDevice);
    SafeRelease(&pD3DDevice);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT curPoint;
            GetCursorPos(&curPoint);
            HMENU hMenu = CreatePopupMenu();

            char path[MAX_PATH];
            if (GetModuleFileNameA(NULL, path, MAX_PATH) != 0) {
                char* lastSlash = strrchr(path, '\\');
                if (lastSlash) *(lastSlash + 1) = '\0';
                lstrcatA(path, "Breathing-config.ini");
            }

            // DYNAMIC PRESET MENU
            for (size_t i = 0; i < g_PresetNames.size(); i++) {
                const char* name = g_PresetNames[i].c_str();

                float inh = GetIniFloat(name, "Inhale", 4.0f, path);
                float hIn = GetIniFloat(name, "HoldIn", 4.0f, path);
                float exh = GetIniFloat(name, "Exhale", 4.0f, path);
                float hEx = GetIniFloat(name, "HoldEx", 4.0f, path);

                char label[128];
                sprintf_s(label, "%s (%.0f-%.0f-%.0f-%.0f)", name, inh, hIn, exh, hEx);

                UINT flags = MF_STRING;
                if (strcmp(g_Config.activePreset, name) == 0) {
                    flags |= MF_CHECKED;
                }

                // Assign ID starting from ID_PRESET_BASE
                AppendMenu(hMenu, flags, ID_PRESET_BASE + i, label);
            }

            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            // Toggle Border Item
            UINT borderFlags = MF_STRING;
            if (g_Config.visuals.showBorder) borderFlags |= MF_CHECKED;
            AppendMenu(hMenu, borderFlags, ID_TOGGLE_BORDER, "Show Border Ring");

            // New Separator & Items
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_OPEN_LOCATION, "Open App Location");
            AppendMenu(hMenu, MF_STRING, ID_EDIT_CONFIG, "Edit Config");
            AppendMenu(hMenu, MF_STRING, ID_TRAY_RELOAD, "Reload Config");
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, curPoint.x, curPoint.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);

            // Handle Dynamic Preset Commands
            if (wmId >= ID_PRESET_BASE && wmId < (int)(ID_PRESET_BASE + g_PresetNames.size())) {
                int index = wmId - ID_PRESET_BASE;
                // Validate index
                if (index >= 0 && index < (int)g_PresetNames.size()) {
                    SavePreset(g_PresetNames[index].c_str());
                    LoadConfig();
                    ResetAnimation();
                }
                break;
            }

            switch(wmId) {
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case ID_TRAY_RELOAD:
                    LoadConfig(); // Presets define new values
                    ResetAnimation();
                    MessageBeep(MB_OK);
                    break;
                case ID_TOGGLE_BORDER:
                    g_Config.visuals.showBorder = !g_Config.visuals.showBorder;
                    SaveSetting("ShowBorder", g_Config.visuals.showBorder ? 1 : 0);
                    // No need for ResetAnimation, just redraw next frame
                    break;

                case ID_EDIT_CONFIG: {
                    char path[MAX_PATH];
                    if (GetModuleFileNameA(NULL, path, MAX_PATH) != 0) {
                        char* lastSlash = strrchr(path, '\\');
                        if (lastSlash) *(lastSlash + 1) = '\0';
                        lstrcatA(path, "Breathing-config.ini");
                        ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOW);
                    }
                } break;

                case ID_OPEN_LOCATION: {
                    char path[MAX_PATH];
                    if (GetModuleFileNameA(NULL, path, MAX_PATH) != 0) {
                        char* lastSlash = strrchr(path, '\\');
                        if (lastSlash) *lastSlash = '\0'; // Truncate at last backslash to get directory
                        ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOW);
                    }
                } break;
            }
        }
        break;

    case WM_DESTROY:
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
