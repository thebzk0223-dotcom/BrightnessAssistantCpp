#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <gdiplus.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr wchar_t APP_NAME[] = L"\u5c4f\u5e55\u4eae\u5ea6\u8c03\u8282\u52a9\u624b";
constexpr wchar_t COPYRIGHT_TEXT[] = L"\u00a9 2026 \u9ec4\u660e\u535a";
constexpr wchar_t ADMIN_OK[] = L"\u7ba1\u7406\u5458\u6743\u9650";
constexpr wchar_t ADMIN_NO[] = L"\u975e\u7ba1\u7406\u5458";
constexpr wchar_t REG_KEY[] = L"Software\\NetworkCenter\\BrightnessAssistant";
constexpr wchar_t TASK_NAME[] = L"\u5c4f\u5e55\u4eae\u5ea6\u8c03\u8282\u52a9\u624b";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT IDT_APPLY = 2001;
constexpr UINT IDT_PREVIEW = 2002;
constexpr UINT IDT_MODE_REAPPLY_1 = 2003;
constexpr UINT IDT_MODE_REAPPLY_2 = 2004;
constexpr int IDC_MODE_BASE = 1000;
constexpr int IDC_STARTUP = 1020;
constexpr int IDC_SAVE = 1021;
constexpr int IDC_ALL_MONITORS = 1022;

constexpr COLORREF C_WHITE = RGB(255, 255, 255);
constexpr COLORREF C_TEXT = RGB(17, 24, 39);
constexpr COLORREF C_MUTED = RGB(92, 101, 116);
constexpr COLORREF C_BORDER = RGB(224, 229, 236);
constexpr COLORREF C_ACCENT = RGB(7, 193, 96);
constexpr COLORREF C_ACCENT_HOVER = RGB(239, 252, 245);

struct ModeInfo {
    const wchar_t* name;
    int brightness;
    int colorTemp;
    double red;
    double green;
    double blue;
};

const std::array<ModeInfo, 4> kModes{{
    {L"\u6807\u51c6", 70, 6500, 1.00, 1.00, 1.00},
    {L"\u62a4\u773c", 60, 5000, 1.00, 0.90, 0.70},
    {L"\u9632\u84dd\u5149", 55, 3800, 1.00, 0.82, 0.45},
    {L"\u6e38\u620f", 85, 7000, 0.96, 1.00, 1.00},
}};

struct Settings {
    int mode = 0;
    int brightness = 70;
    bool startup = false;
    bool saveAndRestore = true;
    bool allMonitors = true;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_modeButtons[4] = {};
HWND g_startupCheck = nullptr;
HWND g_saveCheck = nullptr;
HWND g_allCheck = nullptr;
HWND g_overlay = nullptr;
HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
Settings g_settings;
NOTIFYICONDATAW g_tray{};
bool g_comReady = false;
bool g_draggingSlider = false;
bool g_previewScheduled = false;
double g_sliderPosition = 70.0;
ULONG_PTR g_gdiplusToken = 0;

RECT SliderTrackRect() { return RECT{38, 193, 420, 199}; }
RECT SliderHitRect() { return RECT{28, 176, 432, 219}; }
RECT StartupSwitchRect() { return RECT{24, 272, 404, 298}; }
RECT SaveSwitchRect() { return RECT{24, 304, 404, 330}; }
RECT AllSwitchRect() { return RECT{24, 336, 414, 362}; }

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        HBRUSH brush = CreateSolidBrush(RGB(255, 204, 112));
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ShortExePath() {
    std::wstring path = ExePath();
    DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (needed == 0) return path;
    std::wstring shortPath(needed, L'\0');
    DWORD written = GetShortPathNameW(path.c_str(), &shortPath[0], needed);
    if (written == 0) return path;
    shortPath.resize(written);
    return shortPath;
}

std::wstring Quote(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

std::wstring ProgramDataInstallPath() {
    wchar_t programData[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring base = (len > 0 && len < MAX_PATH) ? programData : L"C:\\ProgramData";
    return base + L"\\NetworkCenterBrightnessAssistant\\BrightnessAssistant.exe";
}

std::wstring EnsureStartupExePath() {
    std::wstring target = ProgramDataInstallPath();
    std::wstring dir = target.substr(0, target.find_last_of(L"\\/"));
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring current = ExePath();
    if (_wcsicmp(current.c_str(), target.c_str()) != 0) {
        CopyFileW(current.c_str(), target.c_str(), FALSE);
    }
    DWORD attrs = GetFileAttributesW(target.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES ? target : current;
}

bool HasArg(const wchar_t* arg) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    if (args) {
        for (int i = 1; i < count; ++i) {
            if (_wcsicmp(args[i], arg) == 0) {
                found = true;
                break;
            }
        }
        LocalFree(args);
    }
    return found;
}

DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    HKEY key{};
    DWORD value = fallback;
    DWORD type = REG_DWORD;
    DWORD size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
    }
    return value;
}

void WriteDword(const wchar_t* name, DWORD value) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

void RunHiddenCommand(const std::wstring& command);

void ConfigureRunFallback(bool enable) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        std::wstring value = Quote(EnsureStartupExePath()) + L" /startup";
        RegSetValueExW(key, TASK_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, TASK_NAME);
    }
    RegCloseKey(key);
}

void DisableStartupArtifacts() {
    ConfigureRunFallback(false);
    RunHiddenCommand(L"schtasks.exe /Delete /TN " + Quote(TASK_NAME) + L" /F");
}

void LoadSettings() {
    g_settings.mode = std::clamp(static_cast<int>(ReadDword(L"Mode", 0)), 0, 3);
    g_settings.brightness = std::clamp(static_cast<int>(ReadDword(L"Brightness", kModes[g_settings.mode].brightness)), 10, 100);
    g_sliderPosition = static_cast<double>(g_settings.brightness);
    g_settings.startup = false;
    g_settings.saveAndRestore = true;
    g_settings.allMonitors = true;
}

void SaveSettings() {
    WriteDword(L"Mode", static_cast<DWORD>(g_settings.mode));
    WriteDword(L"Brightness", static_cast<DWORD>(g_settings.brightness));
    WriteDword(L"Startup", 0);
    WriteDword(L"SaveAndRestore", 1);
    WriteDword(L"AllMonitors", 1);
}

bool IsAdmin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

void RunHiddenCommand(const std::wstring& command) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring mutableCommand = command;
    if (CreateProcessW(nullptr, &mutableCommand[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void ConfigureStartupTask(bool enable) {
    ConfigureRunFallback(enable);
    if (enable) {
        std::wstring tr = EnsureStartupExePath() + L" /startup";
        std::wstring cmd = L"schtasks.exe /Create /TN " + Quote(TASK_NAME) +
                           L" /TR " + Quote(tr) + L" /SC ONLOGON /RL HIGHEST /F";
        RunHiddenCommand(cmd);
    } else {
        RunHiddenCommand(L"schtasks.exe /Delete /TN " + Quote(TASK_NAME) + L" /F");
    }
}

DWORD WINAPI StartupTaskThread(LPVOID param) {
    ConfigureStartupTask(param != nullptr);
    return 0;
}

void ConfigureStartupTaskAsync(bool enable) {
    HANDLE thread = CreateThread(nullptr, 0, StartupTaskThread, enable ? reinterpret_cast<LPVOID>(1) : nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

bool EnsureCom() {
    if (g_comReady) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        g_comReady = true;
    }
    return g_comReady;
}

bool SetInternalBrightness(int percent) {
    if (!EnsureCom()) return false;
    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;
    IEnumWbemClassObject* enumerator = nullptr;
    bool changed = false;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, reinterpret_cast<void**>(&locator));
    if (FAILED(hr)) return false;

    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(ns);
    if (SUCCEEDED(hr)) {
        CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
        hr = services->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 nullptr, &enumerator);
        SysFreeString(wql);
        SysFreeString(query);
    }

    if (SUCCEEDED(hr) && enumerator) {
        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;
        while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned) {
            VARIANT path;
            VariantInit(&path);
            if (SUCCEEDED(obj->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR) {
                IWbemClassObject* methodClass = nullptr;
                IWbemClassObject* inSignature = nullptr;
                IWbemClassObject* inParams = nullptr;
                BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
                BSTR methodName = SysAllocString(L"WmiSetBrightness");
                if (SUCCEEDED(services->GetObject(className, 0, nullptr, &methodClass, nullptr)) &&
                    SUCCEEDED(methodClass->GetMethod(methodName, 0, &inSignature, nullptr)) &&
                    SUCCEEDED(inSignature->SpawnInstance(0, &inParams))) {
                    VARIANT timeout;
                    VARIANT brightness;
                    VariantInit(&timeout);
                    VariantInit(&brightness);
                    timeout.vt = VT_UI4;
                    timeout.uintVal = 1;
                    brightness.vt = VT_UI1;
                    brightness.bVal = static_cast<BYTE>(percent);
                    inParams->Put(L"Timeout", 0, &timeout, 0);
                    inParams->Put(L"Brightness", 0, &brightness, 0);
                    if (SUCCEEDED(services->ExecMethod(path.bstrVal, methodName, 0, nullptr, inParams, nullptr, nullptr))) {
                        changed = true;
                    }
                    inParams->Release();
                }
                if (inSignature) inSignature->Release();
                if (methodClass) methodClass->Release();
                SysFreeString(className);
                SysFreeString(methodName);
            }
            VariantClear(&path);
            obj->Release();
        }
    }

    if (enumerator) enumerator->Release();
    if (services) services->Release();
    locator->Release();
    return changed;
}

struct DdcContext {
    int percent = 70;
    int changed = 0;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<DdcContext*>(param);
    DWORD physicalCount = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &physicalCount) || physicalCount == 0) {
        return TRUE;
    }

    std::vector<PHYSICAL_MONITOR> physical(physicalCount);
    if (GetPhysicalMonitorsFromHMONITOR(monitor, physicalCount, physical.data())) {
        for (auto& item : physical) {
            DWORD minB = 0, curB = 0, maxB = 100;
            if (GetMonitorBrightness(item.hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                DWORD value = minB + static_cast<DWORD>((maxB - minB) * ctx->percent / 100.0);
                if (SetMonitorBrightness(item.hPhysicalMonitor, value)) {
                    ++ctx->changed;
                }
            }
        }
        DestroyPhysicalMonitors(physicalCount, physical.data());
    }
    return TRUE;
}

bool SetExternalBrightness(int percent) {
    DdcContext ctx{percent, 0};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.changed > 0;
}

void ApplyGamma(int brightness, const ModeInfo& mode) {
    WORD ramp[3][256]{};
    double level = std::clamp(brightness / 100.0, 0.12, 1.0);
    const double channel[3] = {mode.red, mode.green, mode.blue};
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 256; ++i) {
            double v = std::clamp(i * 256.0 * level * channel[c], 0.0, 65535.0);
            ramp[c][i] = static_cast<WORD>(v);
        }
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        SetDeviceGammaRamp(screenDc, ramp);
        ReleaseDC(nullptr, screenDc);
    }

    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
            device.cb = sizeof(device);
            continue;
        }
        HDC dc = CreateDCW(device.DeviceName, nullptr, nullptr, nullptr);
        if (dc) {
            SetDeviceGammaRamp(dc, ramp);
            DeleteDC(dc);
        }
        ZeroMemory(&device, sizeof(device));
        device.cb = sizeof(device);
    }
}

void EnsureOverlayWindow() {
    if (g_overlay) return;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"NetworkCenterBrightnessTintOverlay",
        L"",
        WS_POPUP,
        x, y, w, h,
        nullptr,
        nullptr,
        g_instance,
        nullptr);
}

void ApplyOverlayFilter() {
    if (g_settings.mode != 1 && g_settings.mode != 2) {
        if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
        return;
    }

    EnsureOverlayWindow();
    if (!g_overlay) return;

    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BYTE alpha = g_settings.mode == 1 ? 28 : 58;
    SetLayeredWindowAttributes(g_overlay, 0, alpha, LWA_ALPHA);
    InvalidateRect(g_overlay, nullptr, TRUE);
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
}

void ApplyBrightness() {
    const ModeInfo& mode = kModes[g_settings.mode];
    bool hardwareChanged = SetInternalBrightness(g_settings.brightness);
    if (g_settings.allMonitors) {
        hardwareChanged = SetExternalBrightness(g_settings.brightness) || hardwareChanged;
    }
    int gammaBrightness = hardwareChanged && (g_settings.mode == 0 || g_settings.mode == 3)
        ? 100
        : g_settings.brightness;
    ApplyGamma(gammaBrightness, mode);
    ApplyOverlayFilter();
}

DWORD WINAPI ApplyBrightnessThread(LPVOID) {
    ApplyBrightness();
    return 0;
}

void ApplyBrightnessAsync() {
    HANDLE thread = CreateThread(nullptr, 0, ApplyBrightnessThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void ScheduleApply() {
    SetTimer(g_window, IDT_APPLY, 90, nullptr);
}

void SchedulePreview() {
    if (!g_previewScheduled) {
        g_previewScheduled = true;
        SetTimer(g_window, IDT_PREVIEW, 16, nullptr);
    }
}

void ApplyPreviewNow() {
    KillTimer(g_window, IDT_PREVIEW);
    g_previewScheduled = false;
    ApplyGamma(g_settings.brightness, kModes[g_settings.mode]);
    ApplyOverlayFilter();
}

void ScheduleModeReapply() {
    if (g_settings.mode == 1 || g_settings.mode == 2) {
        SetTimer(g_window, IDT_MODE_REAPPLY_1, 180, nullptr);
        SetTimer(g_window, IDT_MODE_REAPPLY_2, 650, nullptr);
    }
}

void InvalidateSliderArea() {
    RECT rc{28, 136, 432, 220};
    InvalidateRect(g_window, &rc, FALSE);
}

void InvalidateSwitchArea(RECT rc) {
    InflateRect(&rc, 2, 2);
    InvalidateRect(g_window, &rc, FALSE);
}

void AddTrayIcon() {
    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
}

void UpdateControls() {
    for (int i = 0; i < 4; ++i) {
        InvalidateRect(g_modeButtons[i], nullptr, FALSE);
    }
    InvalidateRect(g_window, nullptr, FALSE);
}

void SetSliderFromX(int x) {
    RECT rc = SliderTrackRect();
    int width = rc.right - rc.left;
    int clampedX = std::clamp(x, static_cast<int>(rc.left), static_cast<int>(rc.right));
    double visual = 10.0 + (static_cast<double>(clampedX - rc.left) * 90.0 / static_cast<double>(width));
    int value = static_cast<int>(visual + 0.5);
    value = std::clamp(value, 10, 100);
    if (value == g_settings.brightness && std::abs(visual - g_sliderPosition) < 0.01) return;
    g_sliderPosition = visual;
    g_settings.brightness = value;
    InvalidateSliderArea();
    if (g_draggingSlider) {
        UpdateWindow(g_window);
        ApplyPreviewNow();
    }
    SaveSettings();
    if (!g_draggingSlider) {
        ScheduleApply();
    }
}

void SelectMode(int mode) {
    if (mode < 0 || mode > 3) return;
    g_settings.mode = mode;
    g_settings.brightness = kModes[mode].brightness;
    g_sliderPosition = static_cast<double>(g_settings.brightness);
    UpdateControls();
    UpdateWindow(g_window);
    ApplyPreviewNow();
    SaveSettings();
    ApplyBrightness();
    ScheduleModeReapply();
    if ((mode == 0 || mode == 3) && !IsWindowVisible(g_window)) {
        DestroyWindow(g_window);
    }
}

void ToggleStartup() {
    g_settings.startup = !g_settings.startup;
    InvalidateSwitchArea(StartupSwitchRect());
    SaveSettings();
    ConfigureStartupTaskAsync(g_settings.startup);
}

void ToggleSaveAndRestore() {
    g_settings.saveAndRestore = !g_settings.saveAndRestore;
    InvalidateSwitchArea(SaveSwitchRect());
    SaveSettings();
}

void ToggleAllMonitors() {
    g_settings.allMonitors = !g_settings.allMonitors;
    InvalidateSwitchArea(AllSwitchRect());
    SaveSettings();
    ScheduleApply();
}

void MinimizeToTray() {
    ShowWindow(g_window, SW_HIDE);
}

void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"\u663e\u793a\u4e3b\u754c\u9762");
    AppendMenuW(menu, MF_STRING, 2, L"\u5e94\u7528\u4e0a\u6b21\u4eae\u5ea6");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"\u9000\u51fa");
    SetForegroundWindow(g_window);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_window, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) {
        ShowWindow(g_window, SW_SHOW);
        SetForegroundWindow(g_window);
    } else if (cmd == 2) {
        ApplyBrightness();
    } else if (cmd == 3) {
        RemoveTrayIcon();
        DestroyWindow(g_window);
    }
}

void FillRoundRect(HDC dc, RECT rc, int radius, COLORREF fill, COLORREF border = CLR_INVALID) {
    HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? fill : border);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawTextLine(HDC dc, const wchar_t* text, RECT rc, COLORREF color, HFONT font, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rc, format);
    SelectObject(dc, old);
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rc, COLORREF color, HFONT font, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    DrawTextLine(dc, text.c_str(), rc, color, font, format);
}

void DrawSlider(HDC dc) {
    RECT track = SliderTrackRect();
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int x = track.left + static_cast<int>((std::clamp(g_sliderPosition, 10.0, 100.0) - 10.0) * (track.right - track.left) / 90.0 + 0.5);
    int y = (track.top + track.bottom) / 2;

    Gdiplus::Pen basePen(Gdiplus::Color(255, 232, 236, 241), 6.0f);
    basePen.SetStartCap(Gdiplus::LineCapRound);
    basePen.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&basePen, track.left, y, track.right, y);

    Gdiplus::Pen activePen(Gdiplus::Color(255, 7, 193, 96), 6.0f);
    activePen.SetStartCap(Gdiplus::LineCapRound);
    activePen.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&activePen, track.left, y, x, y);

    Gdiplus::SolidBrush knobBrush(Gdiplus::Color(255, 7, 193, 96));
    Gdiplus::Pen knobBorder(Gdiplus::Color(255, 255, 255, 255), 3.0f);
    graphics.FillEllipse(&knobBrush, x - 8, y - 8, 16, 16);
    graphics.DrawEllipse(&knobBorder, x - 8, y - 8, 16, 16);
}

void DrawSwitch(HDC dc, RECT rc, bool checked, const wchar_t* text) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const int x = rc.left + 1;
    const int y = rc.top + 4;
    Gdiplus::SolidBrush bg(checked ? Gdiplus::Color(255, 7, 193, 96) : Gdiplus::Color(255, 214, 220, 228));
    Gdiplus::SolidBrush knob(Gdiplus::Color(255, 255, 255, 255));
    graphics.FillEllipse(&bg, x, y, 18, 18);
    graphics.FillEllipse(&bg, x + 14, y, 18, 18);
    graphics.FillRectangle(&bg, x + 9, y, 14, 18);
    graphics.FillEllipse(&knob, checked ? x + 16 : x + 2, y + 2, 14, 14);

    RECT textRc{rc.left + 42, rc.top, rc.right, rc.bottom};
    DrawTextLine(dc, text, textRc, C_TEXT, g_font);
}

void Paint(HDC dc) {
    RECT rc{};
    GetClientRect(g_window, &rc);
    HBRUSH bg = CreateSolidBrush(C_WHITE);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    FillRoundRect(dc, RECT{8, 8, 448, 340}, 18, C_WHITE, C_BORDER);
    FillRoundRect(dc, RECT{24, 112, 432, 256}, 14, C_WHITE, C_BORDER);

    DrawTextLine(dc, L"\u663e\u793a\u6a21\u5f0f", RECT{24, 26, 140, 50}, C_TEXT, g_boldFont);

    DrawTextLine(dc, L"\u5c4f\u5e55\u4eae\u5ea6", RECT{36, 128, 150, 154}, C_TEXT, g_boldFont);
    std::wstring temp = L"\u8272\u6e29 " + std::to_wstring(kModes[g_settings.mode].colorTemp) + L"K";
    DrawTextLine(dc, temp, RECT{36, 154, 150, 176}, C_MUTED, g_font);
    std::wstring pct = std::to_wstring(g_settings.brightness) + L"%";
    DrawTextLine(dc, pct, RECT{372, 138, 424, 166}, RGB(0, 0, 0), g_titleFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawSlider(dc);
    DrawTextLine(dc, L"\u6697", RECT{36, 226, 66, 248}, C_MUTED, g_font);
    DrawTextLine(dc, L"\u4eae", RECT{402, 226, 424, 248}, C_MUTED, g_font, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    DrawTextLine(dc, COPYRIGHT_TEXT, RECT{24, 304, 180, 328}, C_MUTED, g_font);
    DrawTextLine(dc, IsAdmin() ? ADMIN_OK : ADMIN_NO, RECT{320, 304, 424, 328}, C_MUTED, g_font, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void DrawModeButton(const DRAWITEMSTRUCT* item) {
    int index = static_cast<int>(item->CtlID) - IDC_MODE_BASE;
    bool selected = index == g_settings.mode;
    bool hot = (item->itemState & ODS_HOTLIGHT) != 0;
    COLORREF fill = selected ? C_ACCENT : (hot ? C_ACCENT_HOVER : C_WHITE);
    COLORREF border = selected ? C_ACCENT : RGB(216, 222, 230);
    COLORREF text = selected ? C_WHITE : C_TEXT;
    FillRoundRect(item->hDC, item->rcItem, 12, fill, border);
    DrawTextLine(item->hDC, kModes[index].name, item->rcItem, text, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawCheckBox(const DRAWITEMSTRUCT* item, bool checked, const wchar_t* text) {
    HDC dc = item->hDC;
    HBRUSH white = CreateSolidBrush(C_WHITE);
    FillRect(dc, &item->rcItem, white);
    DeleteObject(white);

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const int x = item->rcItem.left + 1;
    const int y = item->rcItem.top + 4;
    Gdiplus::SolidBrush bg(checked ? Gdiplus::Color(255, 7, 193, 96) : Gdiplus::Color(255, 214, 220, 228));
    Gdiplus::SolidBrush knob(Gdiplus::Color(255, 255, 255, 255));
    graphics.FillEllipse(&bg, x, y, 18, 18);
    graphics.FillEllipse(&bg, x + 14, y, 18, 18);
    graphics.FillRectangle(&bg, x + 9, y, 14, 18);
    graphics.FillEllipse(&knob, checked ? x + 16 : x + 2, y + 2, 14, 14);

    RECT textRc{item->rcItem.left + 42, item->rcItem.top, item->rcItem.right, item->rcItem.bottom};
    DrawTextLine(dc, text, textRc, C_TEXT, g_font);
}

HWND CreateOwnerButton(const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               x, y, w, h, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

void CreateControls(HWND hwnd) {
    g_window = hwnd;
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_boldFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_titleFont = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Microsoft YaHei UI");

    for (int i = 0; i < 4; ++i) {
        g_modeButtons[i] = CreateOwnerButton(kModes[i].name, IDC_MODE_BASE + i, 24 + i * 106, 58, 98, 36);
    }

    g_startupCheck = nullptr;
    g_saveCheck = nullptr;
    g_allCheck = nullptr;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC memDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDc, bitmap));
        Paint(memDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memDc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlID >= IDC_MODE_BASE && item->CtlID < IDC_MODE_BASE + 4) {
            DrawModeButton(item);
        } else if (item->CtlID == IDC_STARTUP) {
            DrawCheckBox(item, g_settings.startup, L"\u5f00\u673a\u81ea\u52a8\u8fd0\u884c\u5e76\u6700\u5c0f\u5316\u5230\u6258\u76d8");
        } else if (item->CtlID == IDC_SAVE) {
            DrawCheckBox(item, g_settings.saveAndRestore, L"\u81ea\u52a8\u4fdd\u5b58\u5e76\u6062\u590d\u6700\u540e\u4e00\u6b21\u8bbe\u7f6e");
        } else if (item->CtlID == IDC_ALL_MONITORS) {
            DrawCheckBox(item, g_settings.allMonitors, L"\u540c\u65f6\u8c03\u8282\u5185\u7f6e\u5c4f\u5e55\u548c\u5916\u63a5\u663e\u793a\u5668");
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= IDC_MODE_BASE && id < IDC_MODE_BASE + 4) {
            SelectMode(id - IDC_MODE_BASE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT hit = SliderHitRect();
        if (PtInRect(&hit, pt)) {
            g_draggingSlider = true;
            SetCapture(hwnd);
            SetSliderFromX(pt.x);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_draggingSlider) {
            SetSliderFromX(GET_X_LPARAM(lp));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_draggingSlider) {
            g_draggingSlider = false;
            ReleaseCapture();
            SetSliderFromX(GET_X_LPARAM(lp));
            if (g_settings.saveAndRestore) SaveSettings();
            ScheduleApply();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_APPLY) {
            KillTimer(hwnd, IDT_APPLY);
            ApplyBrightness();
            return 0;
        } else if (wp == IDT_PREVIEW) {
            KillTimer(hwnd, IDT_PREVIEW);
            g_previewScheduled = false;
            ApplyGamma(g_settings.brightness, kModes[g_settings.mode]);
            ApplyOverlayFilter();
            return 0;
        } else if (wp == IDT_MODE_REAPPLY_1 || wp == IDT_MODE_REAPPLY_2) {
            KillTimer(hwnd, static_cast<UINT_PTR>(wp));
            ApplyGamma(g_settings.brightness, kModes[g_settings.mode]);
            ApplyOverlayFilter();
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        break;
    case WM_CLOSE:
        if (g_settings.mode == 1 || g_settings.mode == 2) {
            ShowWindow(hwnd, SW_HIDE);
            ApplyOverlayFilter();
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_overlay) {
            DestroyWindow(g_overlay);
            g_overlay = nullptr;
        }
        if (g_font) DeleteObject(g_font);
        if (g_boldFont) DeleteObject(g_boldFont);
        if (g_titleFont) DeleteObject(g_titleFont);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = L"NetworkCenterBrightnessAssistantWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClassW(&wc);

    WNDCLASSW overlay{};
    overlay.lpfnWndProc = OverlayProc;
    overlay.hInstance = g_instance;
    overlay.lpszClassName = L"NetworkCenterBrightnessTintOverlay";
    overlay.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    overlay.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&overlay);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\NetworkCenterBrightnessAssistant");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!HasArg(L"/startup")) {
            HWND existing = FindWindowW(L"NetworkCenterBrightnessAssistantWindow", nullptr);
            if (existing) {
                ShowWindow(existing, SW_SHOW);
                SetForegroundWindow(existing);
            }
        }
        if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    LoadSettings();
    RegisterClass();

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"NetworkCenterBrightnessAssistantWindow", APP_NAME,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 472, 388,
                               nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ApplyBrightness();

    if (HasArg(L"/startup")) {
        if (g_settings.mode == 1 || g_settings.mode == 2) {
            ShowWindow(hwnd, SW_HIDE);
        } else {
            DestroyWindow(hwnd);
        }
    } else {
        ConfigureStartupTask(true);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    if (g_comReady) CoUninitialize();
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(msg.wParam);
}
