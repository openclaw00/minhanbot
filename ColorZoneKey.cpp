/*
    ColorZoneKey.cpp - Win32 screen color monitor with keyboard trigger

    Build from a "Developer Command Prompt for VS":
        rc ColorZoneKey.rc
        cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE ColorZoneKey.cpp ColorZoneKey.res user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS

    How to adjust:
        - Defaults are in the CONFIGURATION section below.
        - Runtime values can be edited in the GUI before pressing Start.
        - Target colors are compile-time constants in TARGET_COLORS.

    What it does:
        - Captures a small zone centered on the virtual desktop.
        - Checks every captured pixel against the target RGB colors with a per-channel tolerance.
        - When a match is found, either taps the configured key or holds it until the color disappears.
        - Uses randomized pre-press and key-hold timing to avoid rigid machine-like cadence.
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
constexpr DWORD TOGGLE_HOTKEY = VK_F8;
constexpr int IDI_APP_ICON = 1;

// Capture loop pacing. Use 0 for max-speed polling, but 1-5ms is usually a better CPU/latency tradeoff.
constexpr int CAPTURE_INTERVAL_MS = 2;

// GUI messages.
constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_STATS = WM_APP + 3;
constexpr int TOGGLE_HOTKEY_ID = 1;

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
constexpr int IDC_INTERVAL = 1014;
constexpr int IDC_APPLY = 1015;
constexpr int IDC_HOLD_WHILE_VISIBLE = 1016;
constexpr int IDC_TOGGLE_HOTKEY = 1017;

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
    int captureIntervalMs = CAPTURE_INTERVAL_MS;
    bool holdWhileVisible = false;
    DWORD toggleHotkey = TOGGLE_HOTKEY;
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
    HWND logo = nullptr;
    HWND stats = nullptr;
    DWORD registeredHotkey = 0;

    std::atomic_bool armed{false};
    std::atomic_bool shuttingDown{false};
    std::atomic_int lastHits{0};
    std::atomic_int closestR{0};
    std::atomic_int closestG{0};
    std::atomic_int closestB{0};
    std::atomic_int closestDelta{0};
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

struct DetectionResult {
    bool detected = false;
    int hits = 0;
    RGB_COLOR closest{0, 0, 0};
    int closestDelta = std::numeric_limits<int>::max();
};

DetectionResult AnalyzeTargetColors(const std::uint8_t* bgra, std::size_t pixels, int tolerance) {
    DetectionResult result{};

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t b = bgra[i * 4 + 0];
        const std::uint8_t g = bgra[i * 4 + 1];
        const std::uint8_t r = bgra[i * 4 + 2];

        for (const RGB_COLOR& target : TARGET_COLORS) {
            const int dr = std::abs(static_cast<int>(r) - target.r);
            const int dg = std::abs(static_cast<int>(g) - target.g);
            const int db = std::abs(static_cast<int>(b) - target.b);
            const int maxDelta = std::max({dr, dg, db});

            if (maxDelta < result.closestDelta) {
                result.closestDelta = maxDelta;
                result.closest = {static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
            }

            if (dr <= tolerance && dg <= tolerance && db <= tolerance) {
                ++result.hits;
                result.detected = true;
                break;
            }
        }
    }

    return result;
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

bool SendKeyDown(DWORD vk) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

void SendKeyUp(DWORD vk) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
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
    auto nextStatsUi = std::chrono::steady_clock::now();
    bool keyHeld = false;
    DWORD heldKey = 0;

    while (!g_app.shuttingDown.load(std::memory_order_relaxed)) {
        if (!g_app.armed.load(std::memory_order_relaxed)) {
            if (keyHeld) {
                SendKeyUp(heldKey);
                keyHeld = false;
                heldKey = 0;
            }
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

            const DetectionResult detection = AnalyzeTargetColors(
                capture.data(),
                static_cast<std::size_t>(capture.width()) * static_cast<std::size_t>(capture.height()),
                cfg.tolerance);

            g_app.lastHits.store(detection.hits, std::memory_order_relaxed);
            g_app.closestR.store(detection.closest.r, std::memory_order_relaxed);
            g_app.closestG.store(detection.closest.g, std::memory_order_relaxed);
            g_app.closestB.store(detection.closest.b, std::memory_order_relaxed);
            g_app.closestDelta.store(detection.closestDelta, std::memory_order_relaxed);
            if (std::chrono::steady_clock::now() >= nextStatsUi) {
                PostMessageW(g_app.hwnd, WM_APP_STATS, 0, 0);
                nextStatsUi = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            }

            if (cfg.holdWhileVisible) {
                if (detection.detected) {
                    PostStatus(StatusKind::Detected);

                    if (keyHeld && heldKey != cfg.targetKey) {
                        SendKeyUp(heldKey);
                        keyHeld = false;
                        heldKey = 0;
                    }

                    if (!keyHeld) {
                        std::uniform_int_distribution<int> preDist(cfg.delayBeforePressMin, cfg.delayBeforePressMax);
                        InterruptibleSleepMs(preDist(rng), g_app.armed, g_app.shuttingDown);

                        if (g_app.armed.load(std::memory_order_relaxed) &&
                            !g_app.shuttingDown.load(std::memory_order_relaxed) &&
                            SendKeyDown(cfg.targetKey)) {
                            keyHeld = true;
                            heldKey = cfg.targetKey;
                        }
                    }
                } else {
                    if (keyHeld) {
                        SendKeyUp(heldKey);
                        keyHeld = false;
                        heldKey = 0;
                    }
                    PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
                }
            } else if (detection.detected) {
                if (keyHeld) {
                    SendKeyUp(heldKey);
                    keyHeld = false;
                    heldKey = 0;
                }

                PostStatus(StatusKind::Detected);

                std::uniform_int_distribution<int> preDist(cfg.delayBeforePressMin, cfg.delayBeforePressMax);
                std::uniform_int_distribution<int> holdDist(cfg.keyHoldMin, cfg.keyHoldMax);

                InterruptibleSleepMs(preDist(rng), g_app.armed, g_app.shuttingDown);

                if (g_app.armed.load(std::memory_order_relaxed) &&
                    !g_app.shuttingDown.load(std::memory_order_relaxed)) {
                    SendKeyPress(cfg.targetKey, holdDist(rng), g_app.armed);
                }

                PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
            } else if (keyHeld) {
                SendKeyUp(heldKey);
                keyHeld = false;
                heldKey = 0;
            }
        }

        if (cfg.captureIntervalMs > 0) {
            InterruptibleSleepMs(cfg.captureIntervalMs, g_app.armed, g_app.shuttingDown);
        } else {
            std::this_thread::yield();
        }
    }

    if (keyHeld) {
        SendKeyUp(heldKey);
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

std::wstring VirtualKeyDisplayName(DWORD vk) {
    if (vk >= L'A' && vk <= L'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= L'0' && vk <= L'9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        wchar_t buf[8]{};
        wsprintfW(buf, L"F%u", static_cast<unsigned>(vk - VK_F1 + 1));
        return buf;
    }

    switch (vk) {
    case VK_SPACE: return L"SPACE";
    case VK_RETURN: return L"ENTER";
    case VK_TAB: return L"TAB";
    case VK_ESCAPE: return L"ESC";
    case VK_SHIFT: return L"SHIFT";
    case VK_CONTROL: return L"CTRL";
    case VK_MENU: return L"ALT";
    case VK_LEFT: return L"LEFT";
    case VK_RIGHT: return L"RIGHT";
    case VK_UP: return L"UP";
    case VK_DOWN: return L"DOWN";
    case VK_BACK: return L"BACKSPACE";
    case VK_DELETE: return L"DELETE";
    case VK_HOME: return L"HOME";
    case VK_END: return L"END";
    case VK_PRIOR: return L"PAGEUP";
    case VK_NEXT: return L"PAGEDOWN";
    default:
        wchar_t buf[32]{};
        wsprintfW(buf, L"%u", static_cast<unsigned>(vk));
        return buf;
    }
}

void SetControlKeyName(HWND parent, int id, DWORD value) {
    const std::wstring name = VirtualKeyDisplayName(value);
    SetWindowTextW(GetDlgItem(parent, id), name.c_str());
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
        !ParseIntControl(hwnd, IDC_INTERVAL, next.captureIntervalMs) ||
        !ParseVirtualKeyControl(hwnd, IDC_TOGGLE_HOTKEY, next.toggleHotkey)) {
        error = L"One or more fields are invalid. Keys accept names like SPACE, A, ENTER, or F6.";
        return false;
    }

    if (next.captureWidth < 1 || next.captureWidth > 512 ||
        next.captureHeight < 1 || next.captureHeight > 512) {
        error = L"Capture width and height must be between 1 and 512 px.";
        return false;
    }
    if (next.tolerance < 0 || next.tolerance > 255) {
        error = L"Tolerance must be between 0 and 255 RGB levels.";
        return false;
    }
    if (next.delayBeforePressMin < 0 || next.delayBeforePressMax < next.delayBeforePressMin ||
        next.keyHoldMin < 0 || next.keyHoldMax < next.keyHoldMin) {
        error = L"Each timing range must be non-negative and min <= max.";
        return false;
    }
    if (next.captureIntervalMs < 0 || next.captureIntervalMs > 1000) {
        error = L"Capture interval must be between 0 and 1000 ms.";
        return false;
    }

    next.holdWhileVisible = IsDlgButtonChecked(hwnd, IDC_HOLD_WHILE_VISIBLE) == BST_CHECKED;
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
    SetControlKeyName(hwnd, IDC_KEY, cfg.targetKey);
    SetControlInt(hwnd, IDC_PRE_MIN, cfg.delayBeforePressMin);
    SetControlInt(hwnd, IDC_PRE_MAX, cfg.delayBeforePressMax);
    SetControlInt(hwnd, IDC_HOLD_MIN, cfg.keyHoldMin);
    SetControlInt(hwnd, IDC_HOLD_MAX, cfg.keyHoldMax);
    SetControlInt(hwnd, IDC_INTERVAL, cfg.captureIntervalMs);
    SetControlKeyName(hwnd, IDC_TOGGLE_HOTKEY, cfg.toggleHotkey);
    CheckDlgButton(hwnd, IDC_HOLD_WHILE_VISIBLE, cfg.holdWhileVisible ? BST_CHECKED : BST_UNCHECKED);
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
    HICON logoIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        32,
        32,
        LR_DEFAULTCOLOR | LR_SHARED));
    g_app.logo = CreateWindowExW(0, L"STATIC", nullptr,
                                 WS_CHILD | WS_VISIBLE | SS_ICON,
                                 16, 15, 36, 36,
                                 hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (logoIcon) {
        SendMessageW(g_app.logo, STM_SETICON, reinterpret_cast<WPARAM>(logoIcon), 0);
    }

    g_app.start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  64, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_START), GetModuleHandleW(nullptr), nullptr);
    g_app.stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 160, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_STOP), GetModuleHandleW(nullptr), nullptr);
    g_app.apply = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  256, 16, 86, 30, hwnd, reinterpret_cast<HMENU>(IDC_APPLY), GetModuleHandleW(nullptr), nullptr);

    CreateLabel(hwnd, L"Status", 380, 21, 60, 20);
    g_app.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Disarmed",
                                   WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   450, 16, 140, 30, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), GetModuleHandleW(nullptr), nullptr);

    int y = 66;
    constexpr int labelW = 150;
    constexpr int editW = 90;
    constexpr int smallEditW = 70;
    constexpr int rowH = 24;
    constexpr int gap = 30;
    constexpr int leftLabelX = 16;
    constexpr int leftEditX = 190;
    constexpr int leftUnitX = 292;
    constexpr int rightLabelX = 350;
    constexpr int rightEditX = 530;
    constexpr int rightUnitX = 632;

    CreateLabel(hwnd, L"Capture width", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_WIDTH, leftEditX, y - 3, editW, rowH);
    CreateLabel(hwnd, L"px", leftUnitX, y, 30, rowH);
    CreateLabel(hwnd, L"Capture height", rightLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HEIGHT, rightEditX, y - 3, editW, rowH);
    CreateLabel(hwnd, L"px", rightUnitX, y, 30, rowH);

    y += gap;
    CreateLabel(hwnd, L"Tolerance", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_TOLERANCE, leftEditX, y - 3, editW, rowH);
    CreateLabel(hwnd, L"RGB", leftUnitX, y, 48, rowH);
    CreateLabel(hwnd, L"Key to press", rightLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_KEY, rightEditX, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Delay before key", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_PRE_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_PRE_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);

    y += gap;
    CreateLabel(hwnd, L"Key press length", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HOLD_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_HOLD_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);

    y += gap;
    CreateWindowExW(0, L"BUTTON", L"Hold key while color is visible",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    leftLabelX, y - 2, 300, rowH,
                    hwnd, reinterpret_cast<HMENU>(IDC_HOLD_WHILE_VISIBLE), GetModuleHandleW(nullptr), nullptr);

    y += gap;
    CreateLabel(hwnd, L"Capture interval", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_INTERVAL, leftEditX, y - 3, editW, rowH);
    CreateLabel(hwnd, L"ms", leftUnitX, y, 30, rowH);

    y += gap;
    CreateLabel(hwnd, L"Start/Stop hotkey", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_TOGGLE_HOTKEY, leftEditX, y - 3, editW, rowH);

    CreateLabel(hwnd, L"Live preview", 680, 16, 140, 20);
    g_app.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"ColorZonePreview", L"",
                                    WS_CHILD | WS_VISIBLE,
                                    680, 42, 200, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.stats = CreateWindowExW(0, L"STATIC", L"Hits: 0  Closest: --",
                                  WS_CHILD | WS_VISIBLE,
                                  680, 250, 280, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

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

    if (g_app.registeredHotkey != cfg.toggleHotkey) {
        if (g_app.registeredHotkey != 0) {
            UnregisterHotKey(hwnd, TOGGLE_HOTKEY_ID);
            g_app.registeredHotkey = 0;
        }

        if (!RegisterHotKey(hwnd, TOGGLE_HOTKEY_ID, 0, cfg.toggleHotkey)) {
            MessageBoxW(hwnd, L"That Start/Stop hotkey is already in use. Pick another key.", L"Hotkey unavailable", MB_ICONWARNING | MB_OK);
            return false;
        }
        g_app.registeredHotkey = cfg.toggleHotkey;
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

void ToggleMonitoring(HWND hwnd) {
    if (g_app.armed.load(std::memory_order_relaxed)) {
        StopMonitoring();
    } else {
        StartMonitoring(hwnd);
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        CreateMainControls(hwnd);
        ApplyConfig(hwnd);
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

    case WM_HOTKEY:
        if (static_cast<int>(wParam) == TOGGLE_HOTKEY_ID) {
            ToggleMonitoring(hwnd);
            return 0;
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

    case WM_APP_STATS: {
        wchar_t text[128]{};
        wsprintfW(
            text,
            L"Hits: %d  Closest RGB: %d,%d,%d  +/- %d",
            g_app.lastHits.load(std::memory_order_relaxed),
            g_app.closestR.load(std::memory_order_relaxed),
            g_app.closestG.load(std::memory_order_relaxed),
            g_app.closestB.load(std::memory_order_relaxed),
            g_app.closestDelta.load(std::memory_order_relaxed));
        if (g_app.stats) {
            SetWindowTextW(g_app.stats, text);
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_app.shuttingDown.store(true, std::memory_order_relaxed);
        g_app.armed.store(false, std::memory_order_relaxed);
        if (g_app.registeredHotkey != 0) {
            UnregisterHotKey(hwnd, TOGGLE_HOTKEY_ID);
            g_app.registeredHotkey = 0;
        }
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
    mainClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
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
        990, 360,
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
