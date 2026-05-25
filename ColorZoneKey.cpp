/*
    ColorZoneKey.cpp - Win32 screen color monitor with keyboard trigger

    Build from a "Developer Command Prompt for VS":
        cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE ColorZoneKey.cpp user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS

    How to adjust:
        - Defaults are in the CONFIGURATION section below.
        - Runtime values can be edited in the GUI before pressing Start.
        - Target colors are compile-time constants in TARGET_COLORS.

    What it does:
        - Captures a small zone centered on the virtual desktop.
        - Checks every captured pixel against the target RGB colors with a per-channel tolerance.
        - When a match is found and cooldown has elapsed, sends a configurable keyboard press/release.
        - Uses randomized pre-press, hold, and cooldown timing to avoid rigid machine-like cadence.
        - Runs capture/input on a background worker thread; the GUI remains responsive.

    Notes:
        - This is keyboard-only input injection via SendInput().
        - Compile as a Windows subsystem application; no console window is used.
        - BitBlt into a 32-bit top-down DIB is used because it has very low overhead for tiny regions
          like 40x40 pixels. Desktop Duplication is excellent for full-frame pipelines but adds setup
          and frame-acquisition complexity that usually does not pay off for this small centered zone.
*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <cwctype>
#include <vector>

// === CONFIGURATION ===
constexpr int CAPTURE_WIDTH = 40;
constexpr int CAPTURE_HEIGHT = 40;
constexpr int COLOR_TOLERANCE = 10;
constexpr DWORD TARGET_KEY = VK_SPACE; // Virtual key code

struct RGB_COLOR {
    int r;
    int g;
    int b;
};

// Target colors to detect.
const RGB_COLOR TARGET_COLORS[] = {
    {222, 132, 255},
    {238, 143, 211},
    {253, 118, 255},
    {255, 150, 235}
};

// Humanization / cadence jitter timing (milliseconds).
constexpr int DELAY_BEFORE_PRESS_MIN = 10;
constexpr int DELAY_BEFORE_PRESS_MAX = 50;
constexpr int KEY_HOLD_MIN = 20;
constexpr int KEY_HOLD_MAX = 80;
constexpr int COOLDOWN_MIN = 500;
constexpr int COOLDOWN_MAX = 1500;

// Capture loop pacing. Use 0 for max-speed polling, but 1-5ms is usually a better CPU/latency tradeoff.
constexpr int CAPTURE_INTERVAL_MS = 2;

// GUI messages.
constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;

// Control identifiers.
constexpr int IDC_START = 1001;
constexpr int IDC_STOP = 1002;
constexpr int IDC_STATUS = 1003;
constexpr int IDC_WIDTH = 1004;
constexpr int IDC_HEIGHT = 1005;
constexpr int IDC_TOLERANCE = 1006;
constexpr int IDC_KEY = 1007;
constexpr int IDC_PRE_MIN = 1008;
constexpr int IDC_PRE_MAX = 1009;
constexpr int IDC_HOLD_MIN = 1010;
constexpr int IDC_HOLD_MAX = 1011;
constexpr int IDC_COOL_MIN = 1012;
constexpr int IDC_COOL_MAX = 1013;
constexpr int IDC_INTERVAL = 1014;
constexpr int IDC_APPLY = 1015;

enum class StatusKind : int {
    Disarmed,
    Armed,
    Detected
};

struct RuntimeConfig {
    int captureWidth = CAPTURE_WIDTH;
    int captureHeight = CAPTURE_HEIGHT;
    int tolerance = COLOR_TOLERANCE;
    DWORD targetKey = TARGET_KEY;
    int delayBeforePressMin = DELAY_BEFORE_PRESS_MIN;
    int delayBeforePressMax = DELAY_BEFORE_PRESS_MAX;
    int keyHoldMin = KEY_HOLD_MIN;
    int keyHoldMax = KEY_HOLD_MAX;
    int cooldownMin = COOLDOWN_MIN;
    int cooldownMax = COOLDOWN_MAX;
    int captureIntervalMs = CAPTURE_INTERVAL_MS;
};

struct FrameBuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND preview = nullptr;
    HWND status = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    HWND apply = nullptr;

    std::atomic_bool armed{false};
    std::atomic_bool shuttingDown{false};
    std::thread worker;

    std::mutex configMutex;
    RuntimeConfig config;

    std::mutex frameMutex;
    FrameBuffer latestFrame;
};

AppState g_app;

class ScreenCapture {
public:
    ScreenCapture() = default;

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    ~ScreenCapture() {
        reset();
    }

    bool ensure(int width, int height) {
        if (width == width_ && height == height_ && memDc_ && bits_) {
            return true;
        }

        reset();

        screenDc_ = GetDC(nullptr);
        if (!screenDc_) {
            return false;
        }

        memDc_ = CreateCompatibleDC(screenDc_);
        if (!memDc_) {
            reset();
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down rows, easier preview and scanning
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dib_ = CreateDIBSection(memDc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!dib_ || !bits_) {
            reset();
            return false;
        }

        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
        if (!oldBitmap_) {
            reset();
            return false;
        }

        width_ = width;
        height_ = height;
        return true;
    }

    bool captureCentered() {
        if (!memDc_ || !screenDc_ || !bits_) {
            return false;
        }

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int x = vx + (vw - width_) / 2;
        const int y = vy + (vh - height_) / 2;

        return BitBlt(memDc_, 0, 0, width_, height_, screenDc_, x, y, SRCCOPY | CAPTUREBLT) != FALSE;
    }

    const std::uint8_t* data() const {
        return static_cast<const std::uint8_t*>(bits_);
    }

    int width() const {
        return width_;
    }

    int height() const {
        return height_;
    }

    std::size_t byteSize() const {
        return static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u;
    }

private:
    void reset() {
        if (memDc_ && oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (memDc_) {
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
        if (screenDc_) {
            ReleaseDC(nullptr, screenDc_);
            screenDc_ = nullptr;
        }
        bits_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

    HDC screenDc_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

int ClampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

bool MatchTargetColors(const std::uint8_t* bgra, std::size_t pixels, int tolerance) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t b = bgra[i * 4 + 0];
        const std::uint8_t g = bgra[i * 4 + 1];
        const std::uint8_t r = bgra[i * 4 + 2];

        for (const RGB_COLOR& target : TARGET_COLORS) {
            if (std::abs(static_cast<int>(r) - target.r) <= tolerance &&
                std::abs(static_cast<int>(g) - target.g) <= tolerance &&
                std::abs(static_cast<int>(b) - target.b) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

bool SendKeyPress(DWORD vk, int holdMs, const std::atomic_bool& armed) {
    INPUT inputs[2]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(vk);

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = static_cast<WORD>(vk);
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    if (SendInput(1, &inputs[0], sizeof(INPUT)) != 1) {
        return false;
    }

    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    while (armed.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < end) {
        Sleep(1);
    }

    SendInput(1, &inputs[1], sizeof(INPUT));
    return true;
}

void InterruptibleSleepMs(int ms, const std::atomic_bool& armed, const std::atomic_bool& shuttingDown) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, ms));
    while (!shuttingDown.load(std::memory_order_relaxed) &&
           armed.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < end) {
        Sleep(1);
    }
}

void PostStatus(StatusKind status) {
    if (g_app.hwnd) {
        PostMessageW(g_app.hwnd, WM_APP_STATUS, static_cast<WPARAM>(status), 0);
    }
}

void WorkerMain() {
    ScreenCapture capture;
    std::random_device rd;
    std::mt19937 rng(rd());
    auto nextAllowed = std::chrono::steady_clock::now();

    while (!g_app.shuttingDown.load(std::memory_order_relaxed)) {
        if (!g_app.armed.load(std::memory_order_relaxed)) {
            Sleep(10);
            continue;
        }

        RuntimeConfig cfg;
        {
            std::lock_guard<std::mutex> lock(g_app.configMutex);
            cfg = g_app.config;
        }

        cfg.captureWidth = ClampInt(cfg.captureWidth, 1, 512);
        cfg.captureHeight = ClampInt(cfg.captureHeight, 1, 512);
        cfg.tolerance = ClampInt(cfg.tolerance, 0, 255);

        if (!capture.ensure(cfg.captureWidth, cfg.captureHeight)) {
            PostStatus(StatusKind::Disarmed);
            g_app.armed.store(false, std::memory_order_relaxed);
            Sleep(50);
            continue;
        }

        if (capture.captureCentered()) {
            {
                std::lock_guard<std::mutex> lock(g_app.frameMutex);
                g_app.latestFrame.width = capture.width();
                g_app.latestFrame.height = capture.height();
                g_app.latestFrame.bgra.assign(capture.data(), capture.data() + capture.byteSize());
            }
            if (g_app.preview) {
                PostMessageW(g_app.hwnd, WM_APP_FRAME, 0, 0);
            }

            const bool detected = MatchTargetColors(
                capture.data(),
                static_cast<std::size_t>(capture.width()) * static_cast<std::size_t>(capture.height()),
                cfg.tolerance);

            if (detected && std::chrono::steady_clock::now() >= nextAllowed) {
                PostStatus(StatusKind::Detected);

                std::uniform_int_distribution<int> preDist(cfg.delayBeforePressMin, cfg.delayBeforePressMax);
                std::uniform_int_distribution<int> holdDist(cfg.keyHoldMin, cfg.keyHoldMax);
                std::uniform_int_distribution<int> coolDist(cfg.cooldownMin, cfg.cooldownMax);

                InterruptibleSleepMs(preDist(rng), g_app.armed, g_app.shuttingDown);

                if (g_app.armed.load(std::memory_order_relaxed) &&
                    !g_app.shuttingDown.load(std::memory_order_relaxed)) {
                    SendKeyPress(cfg.targetKey, holdDist(rng), g_app.armed);
                }

                const int cooldownMs = coolDist(rng);
                nextAllowed = std::chrono::steady_clock::now() + std::chrono::milliseconds(cooldownMs);
                PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
            }
        }

        if (cfg.captureIntervalMs > 0) {
            InterruptibleSleepMs(cfg.captureIntervalMs, g_app.armed, g_app.shuttingDown);
        } else {
            std::this_thread::yield();
        }
    }
}

std::wstring GetWindowTextString(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(len) + 1u, L'\0');
    if (len > 0) {
        GetWindowTextW(hwnd, text.data(), len + 1);
    }
    text.resize(static_cast<std::size_t>(len));
    return text;
}

bool ParseIntControl(HWND parent, int id, int& out) {
    const std::wstring text = GetWindowTextString(GetDlgItem(parent, id));
    wchar_t* end = nullptr;
    const long value = wcstol(text.c_str(), &end, 0);
    if (end == text.c_str() || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool ParseDwordControl(HWND parent, int id, DWORD& out) {
    const std::wstring text = GetWindowTextString(GetDlgItem(parent, id));
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 0);
    if (end == text.c_str() || value > 0xFFu) {
        return false;
    }
    out = static_cast<DWORD>(value);
    return true;
}

std::wstring TrimUpper(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return iswspace(ch) != 0;
    }).base();

    if (first >= last) {
        return L"";
    }

    std::wstring result(first, last);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towupper(ch));
    });
    return result;
}

bool ParseVirtualKeyControl(HWND parent, int id, DWORD& out) {
    const std::wstring value = TrimUpper(GetWindowTextString(GetDlgItem(parent, id)));
    if (value.empty()) {
        return false;
    }

    if (value.size() == 1) {
        const wchar_t ch = value[0];
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            out = static_cast<DWORD>(ch);
            return true;
        }
    }

    struct KeyName {
        const wchar_t* name;
        DWORD vk;
    };

    static const KeyName names[] = {
        {L"SPACE", VK_SPACE}, {L"VK_SPACE", VK_SPACE},
        {L"ENTER", VK_RETURN}, {L"RETURN", VK_RETURN}, {L"VK_RETURN", VK_RETURN},
        {L"TAB", VK_TAB}, {L"ESC", VK_ESCAPE}, {L"ESCAPE", VK_ESCAPE},
        {L"SHIFT", VK_SHIFT}, {L"CTRL", VK_CONTROL}, {L"CONTROL", VK_CONTROL}, {L"ALT", VK_MENU},
        {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT}, {L"UP", VK_UP}, {L"DOWN", VK_DOWN},
        {L"BACKSPACE", VK_BACK}, {L"DELETE", VK_DELETE}, {L"DEL", VK_DELETE},
        {L"HOME", VK_HOME}, {L"END", VK_END}, {L"PAGEUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT},
        {L"F1", VK_F1}, {L"F2", VK_F2}, {L"F3", VK_F3}, {L"F4", VK_F4},
        {L"F5", VK_F5}, {L"F6", VK_F6}, {L"F7", VK_F7}, {L"F8", VK_F8},
        {L"F9", VK_F9}, {L"F10", VK_F10}, {L"F11", VK_F11}, {L"F12", VK_F12}
    };

    for (const KeyName& name : names) {
        if (value == name.name) {
            out = name.vk;
            return true;
        }
    }

    wchar_t* end = nullptr;
    const unsigned long numeric = wcstoul(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != L'\0' || numeric > 0xFFu) {
        return false;
    }
    out = static_cast<DWORD>(numeric);
    return true;
}

void SetControlInt(HWND parent, int id, int value) {
    wchar_t buf[32]{};
    wsprintfW(buf, L"%d", value);
    SetWindowTextW(GetDlgItem(parent, id), buf);
}

void SetControlHex(HWND parent, int id, DWORD value) {
    wchar_t buf[32]{};
    wsprintfW(buf, L"0x%02X", value);
    SetWindowTextW(GetDlgItem(parent, id), buf);
}

bool ReadConfigFromControls(HWND hwnd, RuntimeConfig& cfg, std::wstring& error) {
    RuntimeConfig next{};

    if (!ParseIntControl(hwnd, IDC_WIDTH, next.captureWidth) ||
        !ParseIntControl(hwnd, IDC_HEIGHT, next.captureHeight) ||
        !ParseIntControl(hwnd, IDC_TOLERANCE, next.tolerance) ||
        !ParseVirtualKeyControl(hwnd, IDC_KEY, next.targetKey) ||
        !ParseIntControl(hwnd, IDC_PRE_MIN, next.delayBeforePressMin) ||
        !ParseIntControl(hwnd, IDC_PRE_MAX, next.delayBeforePressMax) ||
        !ParseIntControl(hwnd, IDC_HOLD_MIN, next.keyHoldMin) ||
        !ParseIntControl(hwnd, IDC_HOLD_MAX, next.keyHoldMax) ||
        !ParseIntControl(hwnd, IDC_COOL_MIN, next.cooldownMin) ||
        !ParseIntControl(hwnd, IDC_COOL_MAX, next.cooldownMax) ||
        !ParseIntControl(hwnd, IDC_INTERVAL, next.captureIntervalMs)) {
        error = L"One or more fields are invalid. Key accepts names like SPACE, A, F6, or numeric codes like 0x20.";
        return false;
    }

    if (next.captureWidth < 1 || next.captureWidth > 512 ||
        next.captureHeight < 1 || next.captureHeight > 512) {
        error = L"Capture width and height must be between 1 and 512.";
        return false;
    }
    if (next.tolerance < 0 || next.tolerance > 255) {
        error = L"Tolerance must be between 0 and 255.";
        return false;
    }
    if (next.delayBeforePressMin < 0 || next.delayBeforePressMax < next.delayBeforePressMin ||
        next.keyHoldMin < 0 || next.keyHoldMax < next.keyHoldMin ||
        next.cooldownMin < 0 || next.cooldownMax < next.cooldownMin) {
        error = L"Each timing range must be non-negative and min <= max.";
        return false;
    }
    if (next.captureIntervalMs < 0 || next.captureIntervalMs > 1000) {
        error = L"Capture interval must be between 0 and 1000 ms.";
        return false;
    }

    cfg = next;
    return true;
}

void CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, int h) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void PopulateDefaults(HWND hwnd) {
    const RuntimeConfig cfg{};
    SetControlInt(hwnd, IDC_WIDTH, cfg.captureWidth);
    SetControlInt(hwnd, IDC_HEIGHT, cfg.captureHeight);
    SetControlInt(hwnd, IDC_TOLERANCE, cfg.tolerance);
    SetControlHex(hwnd, IDC_KEY, cfg.targetKey);
    SetControlInt(hwnd, IDC_PRE_MIN, cfg.delayBeforePressMin);
    SetControlInt(hwnd, IDC_PRE_MAX, cfg.delayBeforePressMax);
    SetControlInt(hwnd, IDC_HOLD_MIN, cfg.keyHoldMin);
    SetControlInt(hwnd, IDC_HOLD_MAX, cfg.keyHoldMax);
    SetControlInt(hwnd, IDC_COOL_MIN, cfg.cooldownMin);
    SetControlInt(hwnd, IDC_COOL_MAX, cfg.cooldownMax);
    SetControlInt(hwnd, IDC_INTERVAL, cfg.captureIntervalMs);
}

void DrawPreview(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    FrameBuffer frame;
    {
        std::lock_guard<std::mutex> lock(g_app.frameMutex);
        frame = g_app.latestFrame;
    }

    if (frame.width <= 0 || frame.height <= 0 || frame.bgra.empty()) {
        SetTextColor(hdc, RGB(180, 180, 180));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"No preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = frame.width;
    bmi.bmiHeader.biHeight = -frame.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        hdc,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        0, 0, frame.width, frame.height,
        frame.bgra.data(),
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawPreview(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void CreateMainControls(HWND hwnd) {
    g_app.start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  16, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_START), GetModuleHandleW(nullptr), nullptr);
    g_app.stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 112, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_STOP), GetModuleHandleW(nullptr), nullptr);
    g_app.apply = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  208, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_APPLY), GetModuleHandleW(nullptr), nullptr);

    CreateLabel(hwnd, L"Status", 320, 21, 50, 20);
    g_app.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Disarmed",
                                   WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   376, 16, 130, 30, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), GetModuleHandleW(nullptr), nullptr);

    int y = 66;
    constexpr int labelW = 130;
    constexpr int editW = 90;
    constexpr int rowH = 24;
    constexpr int gap = 30;

    CreateLabel(hwnd, L"Capture width", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_WIDTH, 156, y - 3, editW, rowH);
    CreateLabel(hwnd, L"Capture height", 276, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HEIGHT, 416, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Tolerance", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_TOLERANCE, 156, y - 3, editW, rowH);
    CreateLabel(hwnd, L"Target VK", 276, y, labelW, rowH);
    CreateEdit(hwnd, IDC_KEY, 416, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Pre-press min", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_PRE_MIN, 156, y - 3, editW, rowH);
    CreateLabel(hwnd, L"Pre-press max", 276, y, labelW, rowH);
    CreateEdit(hwnd, IDC_PRE_MAX, 416, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Hold min", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HOLD_MIN, 156, y - 3, editW, rowH);
    CreateLabel(hwnd, L"Hold max", 276, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HOLD_MAX, 416, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Cooldown min", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_COOL_MIN, 156, y - 3, editW, rowH);
    CreateLabel(hwnd, L"Cooldown max", 276, y, labelW, rowH);
    CreateEdit(hwnd, IDC_COOL_MAX, 416, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Capture interval", 16, y, labelW, rowH);
    CreateEdit(hwnd, IDC_INTERVAL, 156, y - 3, editW, rowH);

    CreateLabel(hwnd, L"Live preview", 552, 16, 120, 20);
    g_app.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"ColorZonePreview", L"",
                                    WS_CHILD | WS_VISIBLE,
                                    552, 42, 200, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

    PopulateDefaults(hwnd);
    EnableWindow(g_app.stop, FALSE);
}

bool ApplyConfig(HWND hwnd) {
    RuntimeConfig cfg;
    std::wstring error;
    if (!ReadConfigFromControls(hwnd, cfg, error)) {
        MessageBoxW(hwnd, error.c_str(), L"Invalid configuration", MB_ICONWARNING | MB_OK);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_app.configMutex);
        g_app.config = cfg;
    }
    return true;
}

void StartMonitoring(HWND hwnd) {
    if (!ApplyConfig(hwnd)) {
        return;
    }

    g_app.armed.store(true, std::memory_order_relaxed);
    SetWindowTextW(g_app.status, L"Armed");
    EnableWindow(g_app.start, FALSE);
    EnableWindow(g_app.stop, TRUE);
}

void StopMonitoring() {
    g_app.armed.store(false, std::memory_order_relaxed);
    SetWindowTextW(g_app.status, L"Disarmed");
    EnableWindow(g_app.start, TRUE);
    EnableWindow(g_app.stop, FALSE);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        CreateMainControls(hwnd);
        g_app.worker = std::thread(WorkerMain);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_START:
            StartMonitoring(hwnd);
            return 0;
        case IDC_STOP:
            StopMonitoring();
            return 0;
        case IDC_APPLY:
            ApplyConfig(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_APP_FRAME:
        if (g_app.preview) {
            InvalidateRect(g_app.preview, nullptr, FALSE);
        }
        return 0;

    case WM_APP_STATUS:
        switch (static_cast<StatusKind>(wParam)) {
        case StatusKind::Disarmed:
            SetWindowTextW(g_app.status, L"Disarmed");
            break;
        case StatusKind::Armed:
            SetWindowTextW(g_app.status, L"Armed");
            break;
        case StatusKind::Detected:
            SetWindowTextW(g_app.status, L"Detected");
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_app.shuttingDown.store(true, std::memory_order_relaxed);
        g_app.armed.store(false, std::memory_order_relaxed);
        if (g_app.worker.joinable()) {
            g_app.worker.join();
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    WNDCLASSW previewClass{};
    previewClass.lpfnWndProc = PreviewProc;
    previewClass.hInstance = hInstance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.lpszClassName = L"ColorZonePreview";
    previewClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&previewClass)) {
        return 1;
    }

    WNDCLASSW mainClass{};
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = hInstance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.lpszClassName = L"ColorZoneKeyWindow";
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&mainClass)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        mainClass.lpszClassName,
        L"Color Zone Key Monitor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        790, 300,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
