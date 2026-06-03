/*
    minhanbot - Win32 screen color monitor with keyboard trigger

    Build from a "Developer Command Prompt for VS":
        rc minhanbot.rc
        cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /Fe:minhanbot.exe minhanbot.cpp minhanbot.res user32.lib gdi32.lib d3d11.lib dxgi.lib /link /SUBSYSTEM:WINDOWS

    How to adjust:
        - Defaults are in the CONFIGURATION section below.
        - Runtime values can be edited in the GUI before pressing Start.
        - Target color can be sampled from the screen with the GUI picker.

    What it does:
        - Captures a small zone centered on the virtual desktop.
        - Checks every captured pixel against the target RGB colors with a per-channel tolerance.
        - Requires a configurable number of matching color pixels before triggering.
        - When a match is found, either taps the configured key or holds it until the color disappears.
        - Uses randomized pre-press, key-hold, release, and scan timing to avoid rigid cadence.
        - Runs capture/serial-command output on a background worker thread; the GUI remains responsive.

    Notes:
        - Key commands are sent only to an external serial USB HID bridge.
        - Compile as a Windows subsystem application; no console window is used.
        - DXGI Desktop Duplication is used to capture the output containing the center of the
          virtual desktop, then a small centered region is copied into a CPU-readable texture.
*/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <cwctype>
#include <vector>

// === CONFIGURATION ===
constexpr int CAPTURE_WIDTH = 25;
constexpr int CAPTURE_HEIGHT = 25;
constexpr int COLOR_TOLERANCE = 40;
constexpr int MIN_COLOR_PIXELS = 1;
constexpr DWORD TARGET_KEY = L'J'; // Virtual key code
constexpr int TARGET_COLOR_R = 222;
constexpr int TARGET_COLOR_G = 132;
constexpr int TARGET_COLOR_B = 255;

struct RGB_COLOR {
    int r;
    int g;
    int b;
};

// Humanization / cadence jitter timing (milliseconds).
constexpr int DELAY_BEFORE_PRESS_MIN = 100;
constexpr int DELAY_BEFORE_PRESS_MAX = 175;
constexpr int KEY_HOLD_MIN = 20;
constexpr int KEY_HOLD_MAX = 100;
constexpr int COOLDOWN_AFTER_PRESS_MIN = 0;
constexpr int COOLDOWN_AFTER_PRESS_MAX = 0;
constexpr int COOLDOWN_AFTER_PRESS_EVERY = 1;
constexpr int RELEASE_DELAY_MIN = 20;
constexpr int RELEASE_DELAY_MAX = 100;
constexpr DWORD TOGGLE_HOTKEY = VK_F8;
constexpr int IDI_APP_ICON = 1;

// Capture loop pacing. Use 0 for max-speed polling.
constexpr int SCAN_INTERVAL_MIN = 7;
constexpr int SCAN_INTERVAL_MAX = 13;

// GUI messages.
constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_STATS = WM_APP + 3;
constexpr UINT WM_APP_PICKED_COLOR = WM_APP + 4;
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
constexpr int IDC_RELEASE_MIN = 1012;
constexpr int IDC_RELEASE_MAX = 1013;
constexpr int IDC_SCAN_MIN = 1014;
constexpr int IDC_APPLY = 1015;
constexpr int IDC_HOLD_WHILE_VISIBLE = 1016;
constexpr int IDC_TOGGLE_HOTKEY = 1017;
constexpr int IDC_SERIAL_PORT = 1019;
constexpr int IDC_MIN_COLOR_PIXELS = 1020;
constexpr int IDC_SCAN_MAX = 1021;
constexpr int IDC_PICK_COLOR = 1022;
constexpr int IDC_TARGET_COLOR = 1023;
constexpr int IDC_COOLDOWN_MIN = 1024;
constexpr int IDC_COOLDOWN_MAX = 1025;
constexpr int IDC_COOLDOWN_EVERY = 1026;
constexpr int IDC_REQUIRE_HELD_INPUT = 1027;
constexpr int IDC_HELD_INPUT_KEY = 1028;

// Black-and-white dark UI theme.
constexpr COLORREF THEME_BG = RGB(8, 8, 8);
constexpr COLORREF THEME_PANEL = RGB(18, 18, 18);
constexpr COLORREF THEME_FIELD = RGB(12, 12, 12);
constexpr COLORREF THEME_FIELD_ALT = RGB(28, 28, 28);
constexpr COLORREF THEME_BORDER = RGB(82, 82, 82);
constexpr COLORREF THEME_BORDER_HOT = RGB(170, 170, 170);
constexpr COLORREF THEME_TEXT = RGB(245, 245, 245);
constexpr COLORREF THEME_MUTED = RGB(178, 178, 178);
constexpr COLORREF THEME_DISABLED = RGB(94, 94, 94);

enum class StatusKind : int {
    Disarmed,
    Armed,
    Detected
};

struct RuntimeConfig {
    int captureWidth = CAPTURE_WIDTH;
    int captureHeight = CAPTURE_HEIGHT;
    int tolerance = COLOR_TOLERANCE;
    int minColorPixels = MIN_COLOR_PIXELS;
    RGB_COLOR targetColor{TARGET_COLOR_R, TARGET_COLOR_G, TARGET_COLOR_B};
    DWORD targetKey = TARGET_KEY;
    int delayBeforePressMin = DELAY_BEFORE_PRESS_MIN;
    int delayBeforePressMax = DELAY_BEFORE_PRESS_MAX;
    int keyHoldMin = KEY_HOLD_MIN;
    int keyHoldMax = KEY_HOLD_MAX;
    int cooldownAfterPressMin = COOLDOWN_AFTER_PRESS_MIN;
    int cooldownAfterPressMax = COOLDOWN_AFTER_PRESS_MAX;
    int cooldownAfterPressEvery = COOLDOWN_AFTER_PRESS_EVERY;
    int releaseDelayMin = RELEASE_DELAY_MIN;
    int releaseDelayMax = RELEASE_DELAY_MAX;
    int scanIntervalMin = SCAN_INTERVAL_MIN;
    int scanIntervalMax = SCAN_INTERVAL_MAX;
    bool holdWhileVisible = true;
    bool requireHeldInput = false;
    DWORD heldInputKey = VK_RBUTTON;
    DWORD toggleHotkey = TOGGLE_HOTKEY;
    std::wstring serialPort = L"COM4";
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
    HWND stats = nullptr;
    HWND targetColorText = nullptr;
    DWORD registeredHotkey = 0;

    std::atomic_bool armed{false};
    std::atomic_bool shuttingDown{false};
    std::atomic_bool colorPickActive{false};
    std::atomic_int lastHits{0};
    std::atomic_int requiredHits{MIN_COLOR_PIXELS};
    std::atomic_int closestR{0};
    std::atomic_int closestG{0};
    std::atomic_int closestB{0};
    std::atomic_int closestDelta{0};
    std::thread worker;

    std::mutex configMutex;
    RuntimeConfig config;
    RGB_COLOR selectedTargetColor{TARGET_COLOR_R, TARGET_COLOR_G, TARGET_COLOR_B};

    std::mutex frameMutex;
    FrameBuffer latestFrame;
};

AppState g_app;

HBRUSH ThemeBackgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(THEME_BG);
    return brush;
}

HBRUSH ThemePanelBrush() {
    static HBRUSH brush = CreateSolidBrush(THEME_PANEL);
    return brush;
}

HBRUSH ThemeFieldBrush() {
    static HBRUSH brush = CreateSolidBrush(THEME_FIELD);
    return brush;
}

HBRUSH ThemeListBrush() {
    static HBRUSH brush = CreateSolidBrush(THEME_FIELD_ALT);
    return brush;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

int ClampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

class ScreenCapture {
public:
    ScreenCapture() = default;

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    ~ScreenCapture() {
        reset();
    }

    bool ensure(int width, int height) {
        if (width == width_ && height == height_ && duplication_ && staging_ && !bgra_.empty()) {
            return true;
        }

        reset();

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device_,
            nullptr,
            &context_);
        if (FAILED(hr)) {
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (SUCCEEDED(hr)) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);
        if (FAILED(hr) || !adapter) {
            reset();
            return false;
        }

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int centerX = vx + vw / 2;
        const int centerY = vy + vh / 2;

        IDXGIOutput* selectedOutput = nullptr;
        DXGI_OUTPUT_DESC selectedDesc{};
        for (UINT index = 0; ; ++index) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC desc{};
            if (output && SUCCEEDED(output->GetDesc(&desc))) {
                const RECT& rc = desc.DesktopCoordinates;
                if (centerX >= rc.left && centerX < rc.right && centerY >= rc.top && centerY < rc.bottom) {
                    selectedOutput = output;
                    selectedDesc = desc;
                    break;
                }
            }
            SafeRelease(output);
        }
        SafeRelease(adapter);

        if (!selectedOutput) {
            reset();
            return false;
        }

        IDXGIOutput1* output1 = nullptr;
        hr = selectedOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        SafeRelease(selectedOutput);
        if (FAILED(hr) || !output1) {
            reset();
            return false;
        }

        hr = output1->DuplicateOutput(device_, &duplication_);
        SafeRelease(output1);
        if (FAILED(hr)) {
            reset();
            return false;
        }

        outputLeft_ = selectedDesc.DesktopCoordinates.left;
        outputTop_ = selectedDesc.DesktopCoordinates.top;
        outputWidth_ = selectedDesc.DesktopCoordinates.right - selectedDesc.DesktopCoordinates.left;
        outputHeight_ = selectedDesc.DesktopCoordinates.bottom - selectedDesc.DesktopCoordinates.top;
        if (width > outputWidth_ || height > outputHeight_) {
            reset();
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_STAGING;
        textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        hr = device_->CreateTexture2D(&textureDesc, nullptr, &staging_);
        if (FAILED(hr)) {
            reset();
            return false;
        }

        width_ = width;
        height_ = height;
        bgra_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u, 0);
        return true;
    }

    bool captureCentered() {
        if (!duplication_ || !context_ || !staging_) {
            return false;
        }

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int desiredX = vx + (vw - width_) / 2;
        const int desiredY = vy + (vh - height_) / 2;
        const int localX = ClampInt(desiredX - outputLeft_, 0, outputWidth_ - width_);
        const int localY = ClampInt(desiredY - outputTop_, 0, outputHeight_ - height_);

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* frameResource = nullptr;
        HRESULT hr = duplication_->AcquireNextFrame(0, &frameInfo, &frameResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return !bgra_.empty();
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            reset();
            return false;
        }
        if (FAILED(hr) || !frameResource) {
            return false;
        }

        ID3D11Texture2D* frameTexture = nullptr;
        hr = frameResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frameTexture));
        SafeRelease(frameResource);
        if (FAILED(hr) || !frameTexture) {
            duplication_->ReleaseFrame();
            return false;
        }

        D3D11_BOX srcBox{};
        srcBox.left = static_cast<UINT>(localX);
        srcBox.top = static_cast<UINT>(localY);
        srcBox.front = 0;
        srcBox.right = static_cast<UINT>(localX + width_);
        srcBox.bottom = static_cast<UINT>(localY + height_);
        srcBox.back = 1;
        context_->CopySubresourceRegion(staging_, 0, 0, 0, 0, frameTexture, 0, &srcBox);
        SafeRelease(frameTexture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            const std::uint8_t* src = static_cast<const std::uint8_t*>(mapped.pData);
            const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4u;
            for (int row = 0; row < height_; ++row) {
                std::memcpy(
                    bgra_.data() + static_cast<std::size_t>(row) * rowBytes,
                    src + static_cast<std::size_t>(row) * mapped.RowPitch,
                    rowBytes);
            }
            context_->Unmap(staging_, 0);
        }

        duplication_->ReleaseFrame();
        return SUCCEEDED(hr);
    }

    const std::uint8_t* data() const {
        return bgra_.data();
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
        SafeRelease(staging_);
        SafeRelease(duplication_);
        SafeRelease(context_);
        SafeRelease(device_);
        bgra_.clear();
        width_ = 0;
        height_ = 0;
        outputLeft_ = 0;
        outputTop_ = 0;
        outputWidth_ = 0;
        outputHeight_ = 0;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    std::vector<std::uint8_t> bgra_;
    int width_ = 0;
    int height_ = 0;
    int outputLeft_ = 0;
    int outputTop_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
};

int SampleBellCurveMs(std::mt19937& rng, int minMs, int maxMs) {
    if (maxMs <= minMs) {
        return minMs;
    }

    const double mean = (static_cast<double>(minMs) + static_cast<double>(maxMs)) / 2.0;
    const double stddev = static_cast<double>(maxMs - minMs) / 6.0;
    std::normal_distribution<double> dist(mean, stddev);

    for (int attempt = 0; attempt < 8; ++attempt) {
        const int sample = static_cast<int>(dist(rng) + 0.5);
        if (sample >= minMs && sample <= maxMs) {
            return sample;
        }
    }

    return ClampInt(static_cast<int>(dist(rng) + 0.5), minMs, maxMs);
}

struct DetectionResult {
    bool detected = false;
    int hits = 0;
    RGB_COLOR closest{0, 0, 0};
    int closestDelta = std::numeric_limits<int>::max();
};

DetectionResult AnalyzeTargetColors(
    const std::uint8_t* bgra,
    std::size_t pixels,
    const RGB_COLOR& target,
    int tolerance,
    int minColorPixels) {
    DetectionResult result{};

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t b = bgra[i * 4 + 0];
        const std::uint8_t g = bgra[i * 4 + 1];
        const std::uint8_t r = bgra[i * 4 + 2];

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
        }
    }

    result.detected = result.hits >= minColorPixels;
    return result;
}

class SerialHidBridge {
public:
    SerialHidBridge() = default;

    SerialHidBridge(const SerialHidBridge&) = delete;
    SerialHidBridge& operator=(const SerialHidBridge&) = delete;

    ~SerialHidBridge() {
        close();
    }

    bool send(bool down, DWORD vk, const std::wstring& port) {
        if (!ensureOpen(port)) {
            return false;
        }

        char command[32]{};
        std::snprintf(command, sizeof(command), "%c %02X\n", down ? 'D' : 'U', static_cast<unsigned>(vk & 0xFFu));

        DWORD written = 0;
        const DWORD length = static_cast<DWORD>(std::strlen(command));
        if (!WriteFile(handle_, command, length, &written, nullptr) || written != length) {
            close();
            return false;
        }
        return true;
    }

private:
    bool ensureOpen(const std::wstring& port) {
        if (handle_ != INVALID_HANDLE_VALUE && port == openPort_) {
            return true;
        }

        close();

        std::wstring path = port;
        if (path.rfind(L"\\\\.\\", 0) != 0) {
            path = L"\\\\.\\" + path;
        }

        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            close();
            return false;
        }
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(handle_, &dcb)) {
            close();
            return false;
        }

        COMMTIMEOUTS timeouts{};
        timeouts.WriteTotalTimeoutConstant = 20;
        timeouts.WriteTotalTimeoutMultiplier = 1;
        SetCommTimeouts(handle_, &timeouts);

        openPort_ = port;
        return true;
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        openPort_.clear();
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring openPort_;
};

class KeyOutput {
public:
    bool down(const RuntimeConfig& cfg, DWORD vk) {
        return serial_.send(true, vk, cfg.serialPort);
    }

    void up(const RuntimeConfig& cfg, DWORD vk) {
        serial_.send(false, vk, cfg.serialPort);
    }

    bool press(const RuntimeConfig& cfg, DWORD vk, int holdMs, const std::atomic_bool& armed) {
        if (!down(cfg, vk)) {
            return false;
        }

        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
        while (armed.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < end) {
            Sleep(1);
        }

        up(cfg, vk);
        return true;
    }

private:
    SerialHidBridge serial_;
};

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

bool RequiredHeldInputActive(const RuntimeConfig& cfg) {
    if (!cfg.requireHeldInput) {
        return true;
    }
    return (GetAsyncKeyState(static_cast<int>(cfg.heldInputKey)) & 0x8000) != 0;
}

void WorkerMain() {
    ScreenCapture capture;
    KeyOutput output;
    std::random_device rd;
    std::mt19937 rng(rd());
    auto nextStatsUi = std::chrono::steady_clock::now();
    bool keyHeld = false;
    DWORD heldKey = 0;
    RuntimeConfig heldConfig;
    bool releasePending = false;
    auto releaseAt = std::chrono::steady_clock::now();
    int nonHoldPressesSinceCooldown = 0;

    while (!g_app.shuttingDown.load(std::memory_order_relaxed)) {
        if (!g_app.armed.load(std::memory_order_relaxed)) {
            if (keyHeld) {
                output.up(heldConfig, heldKey);
                keyHeld = false;
                heldKey = 0;
            }
            releasePending = false;
            nonHoldPressesSinceCooldown = 0;
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
        cfg.minColorPixels = ClampInt(cfg.minColorPixels, 1, cfg.captureWidth * cfg.captureHeight);

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
                cfg.targetColor,
                cfg.tolerance,
                cfg.minColorPixels);

            g_app.lastHits.store(detection.hits, std::memory_order_relaxed);
            g_app.requiredHits.store(cfg.minColorPixels, std::memory_order_relaxed);
            g_app.closestR.store(detection.closest.r, std::memory_order_relaxed);
            g_app.closestG.store(detection.closest.g, std::memory_order_relaxed);
            g_app.closestB.store(detection.closest.b, std::memory_order_relaxed);
            g_app.closestDelta.store(detection.closestDelta, std::memory_order_relaxed);
            if (std::chrono::steady_clock::now() >= nextStatsUi) {
                PostMessageW(g_app.hwnd, WM_APP_STATS, 0, 0);
                nextStatsUi = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            }

            const bool heldInputActive = RequiredHeldInputActive(cfg);

            if (cfg.holdWhileVisible) {
                nonHoldPressesSinceCooldown = 0;
                if (detection.detected && heldInputActive) {
                    PostStatus(StatusKind::Detected);
                    releasePending = false;

                    if (keyHeld && heldKey != cfg.targetKey) {
                        output.up(heldConfig, heldKey);
                        keyHeld = false;
                        heldKey = 0;
                        releasePending = false;
                    }

                    if (!keyHeld) {
                        InterruptibleSleepMs(
                            SampleBellCurveMs(rng, cfg.delayBeforePressMin, cfg.delayBeforePressMax),
                            g_app.armed,
                            g_app.shuttingDown);

                        if (g_app.armed.load(std::memory_order_relaxed) &&
                            !g_app.shuttingDown.load(std::memory_order_relaxed) &&
                            RequiredHeldInputActive(cfg) &&
                            output.down(cfg, cfg.targetKey)) {
                            keyHeld = true;
                            heldKey = cfg.targetKey;
                            heldConfig = cfg;
                        }
                    }
                } else if (keyHeld && detection.detected && !heldInputActive) {
                    output.up(heldConfig, heldKey);
                    keyHeld = false;
                    heldKey = 0;
                    releasePending = false;
                    PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
                } else {
                    if (keyHeld) {
                        if (!releasePending) {
                            std::uniform_int_distribution<int> releaseDist(cfg.releaseDelayMin, cfg.releaseDelayMax);
                            releaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(releaseDist(rng));
                            releasePending = true;
                        }

                        if (std::chrono::steady_clock::now() >= releaseAt) {
                            output.up(heldConfig, heldKey);
                            keyHeld = false;
                            heldKey = 0;
                            releasePending = false;
                        }
                    }
                    PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
                }
            } else if (detection.detected && heldInputActive) {
                if (keyHeld) {
                    output.up(heldConfig, heldKey);
                    keyHeld = false;
                    heldKey = 0;
                    releasePending = false;
                }

                PostStatus(StatusKind::Detected);

                std::uniform_int_distribution<int> holdDist(cfg.keyHoldMin, cfg.keyHoldMax);

                InterruptibleSleepMs(
                    SampleBellCurveMs(rng, cfg.delayBeforePressMin, cfg.delayBeforePressMax),
                    g_app.armed,
                    g_app.shuttingDown);

                if (g_app.armed.load(std::memory_order_relaxed) &&
                    !g_app.shuttingDown.load(std::memory_order_relaxed) &&
                    RequiredHeldInputActive(cfg)) {
                    if (output.press(cfg, cfg.targetKey, holdDist(rng), g_app.armed)) {
                        ++nonHoldPressesSinceCooldown;
                    }
                }

                if (nonHoldPressesSinceCooldown >= cfg.cooldownAfterPressEvery) {
                    nonHoldPressesSinceCooldown = 0;
                    InterruptibleSleepMs(
                        SampleBellCurveMs(rng, cfg.cooldownAfterPressMin, cfg.cooldownAfterPressMax),
                        g_app.armed,
                        g_app.shuttingDown);
                }

                PostStatus(g_app.armed.load(std::memory_order_relaxed) ? StatusKind::Armed : StatusKind::Disarmed);
            } else if (keyHeld) {
                output.up(heldConfig, heldKey);
                keyHeld = false;
                heldKey = 0;
            }
        }

        const int scanIntervalMs = SampleBellCurveMs(rng, cfg.scanIntervalMin, cfg.scanIntervalMax);
        if (scanIntervalMs > 0) {
            InterruptibleSleepMs(scanIntervalMs, g_app.armed, g_app.shuttingDown);
        } else {
            std::this_thread::yield();
        }
    }

    if (keyHeld) {
        output.up(heldConfig, heldKey);
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

bool ParseVirtualKeyControl(HWND parent, int id, DWORD& out, bool allowMouseButtons = false) {
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

    if (allowMouseButtons) {
        static const KeyName mouseNames[] = {
            {L"LBUTTON", VK_LBUTTON}, {L"LEFTCLICK", VK_LBUTTON}, {L"LEFT_CLICK", VK_LBUTTON},
            {L"RBUTTON", VK_RBUTTON}, {L"RIGHTCLICK", VK_RBUTTON}, {L"RIGHT_CLICK", VK_RBUTTON},
            {L"MBUTTON", VK_MBUTTON}, {L"MIDDLECLICK", VK_MBUTTON}, {L"MIDDLE_CLICK", VK_MBUTTON}
        };

        for (const KeyName& name : mouseNames) {
            if (value == name.name) {
                out = name.vk;
                return true;
            }
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

void SetTargetColorText(HWND hwnd, const RGB_COLOR& color) {
    wchar_t text[64]{};
    wsprintfW(text, L"RGB: %d,%d,%d", color.r, color.g, color.b);
    SetWindowTextW(GetDlgItem(hwnd, IDC_TARGET_COLOR), text);
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
    case VK_LBUTTON: return L"LBUTTON";
    case VK_RBUTTON: return L"RBUTTON";
    case VK_MBUTTON: return L"MBUTTON";
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

bool IsSerialPortNameValid(const std::wstring& value) {
    if (value.size() < 4 || value.size() > 10) {
        return false;
    }
    if (value.rfind(L"COM", 0) != 0) {
        return false;
    }
    for (std::size_t i = 3; i < value.size(); ++i) {
        if (iswdigit(value[i]) == 0) {
            return false;
        }
    }
    return true;
}

bool ReadConfigFromControls(HWND hwnd, RuntimeConfig& cfg, std::wstring& error) {
    RuntimeConfig next{};

    if (!ParseIntControl(hwnd, IDC_WIDTH, next.captureWidth) ||
        !ParseIntControl(hwnd, IDC_HEIGHT, next.captureHeight) ||
        !ParseIntControl(hwnd, IDC_TOLERANCE, next.tolerance) ||
        !ParseIntControl(hwnd, IDC_MIN_COLOR_PIXELS, next.minColorPixels) ||
        !ParseVirtualKeyControl(hwnd, IDC_KEY, next.targetKey) ||
        !ParseIntControl(hwnd, IDC_PRE_MIN, next.delayBeforePressMin) ||
        !ParseIntControl(hwnd, IDC_PRE_MAX, next.delayBeforePressMax) ||
        !ParseIntControl(hwnd, IDC_HOLD_MIN, next.keyHoldMin) ||
        !ParseIntControl(hwnd, IDC_HOLD_MAX, next.keyHoldMax) ||
        !ParseIntControl(hwnd, IDC_COOLDOWN_MIN, next.cooldownAfterPressMin) ||
        !ParseIntControl(hwnd, IDC_COOLDOWN_MAX, next.cooldownAfterPressMax) ||
        !ParseIntControl(hwnd, IDC_COOLDOWN_EVERY, next.cooldownAfterPressEvery) ||
        !ParseIntControl(hwnd, IDC_RELEASE_MIN, next.releaseDelayMin) ||
        !ParseIntControl(hwnd, IDC_RELEASE_MAX, next.releaseDelayMax) ||
        !ParseIntControl(hwnd, IDC_SCAN_MIN, next.scanIntervalMin) ||
        !ParseIntControl(hwnd, IDC_SCAN_MAX, next.scanIntervalMax) ||
        !ParseVirtualKeyControl(hwnd, IDC_HELD_INPUT_KEY, next.heldInputKey, true) ||
        !ParseVirtualKeyControl(hwnd, IDC_TOGGLE_HOTKEY, next.toggleHotkey)) {
        error = L"One or more fields are invalid. Key to press accepts keyboard keys; Held input also accepts RBUTTON.";
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
    if (next.minColorPixels < 1 || next.minColorPixels > next.captureWidth * next.captureHeight) {
        error = L"Required color pixels must be between 1 and the capture area's total pixels.";
        return false;
    }
    if (next.delayBeforePressMin < 0 || next.delayBeforePressMax < next.delayBeforePressMin ||
        next.keyHoldMin < 0 || next.keyHoldMax < next.keyHoldMin ||
        next.cooldownAfterPressMin < 0 || next.cooldownAfterPressMax < next.cooldownAfterPressMin ||
        next.releaseDelayMin < 0 || next.releaseDelayMax < next.releaseDelayMin) {
        error = L"Each timing range must be non-negative and min <= max.";
        return false;
    }
    if (next.cooldownAfterPressEvery < 1 || next.cooldownAfterPressEvery > 1000) {
        error = L"Cooldown every must be between 1 and 1000 activations.";
        return false;
    }
    if (next.scanIntervalMin < 0 || next.scanIntervalMax < next.scanIntervalMin || next.scanIntervalMax > 1000) {
        error = L"Scan interval range must be between 0 and 1000 ms with min <= max.";
        return false;
    }

    next.targetColor = g_app.selectedTargetColor;
    next.holdWhileVisible = IsDlgButtonChecked(hwnd, IDC_HOLD_WHILE_VISIBLE) == BST_CHECKED;
    next.requireHeldInput = IsDlgButtonChecked(hwnd, IDC_REQUIRE_HELD_INPUT) == BST_CHECKED;
    next.serialPort = TrimUpper(GetWindowTextString(GetDlgItem(hwnd, IDC_SERIAL_PORT)));
    if (!IsSerialPortNameValid(next.serialPort)) {
        error = L"External Arduino input needs a COM port like COM4.";
        return false;
    }

    cfg = next;
    return true;
}

void CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, int h) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND CreateCombo(HWND parent, int id, int x, int y, int w, int h) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void PopulateHitThresholdChoices(HWND combo) {
    const int choices[] = {1, 2, 3, 5, 10, 15, 25, 50, 100, 250, 500};
    for (int value : choices) {
        wchar_t text[16]{};
        wsprintfW(text, L"%d", value);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }
}

void PopulateSerialPortChoices(HWND combo) {
    for (int port = 1; port <= 20; ++port) {
        wchar_t text[16]{};
        wsprintfW(text, L"COM%d", port);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }
}

void DrawButtonControl(const DRAWITEMSTRUCT& item) {
    HDC hdc = item.hDC;
    RECT rc = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    HBRUSH fill = CreateSolidBrush(disabled ? RGB(20, 20, 20) : (pressed ? RGB(36, 36, 36) : THEME_PANEL));
    FillRect(hdc, &rc, fill);
    DeleteObject(fill);

    HPEN border = CreatePen(PS_SOLID, 1, focused ? THEME_BORDER_HOT : THEME_BORDER);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(sizeof(text) / sizeof(text[0])));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? THEME_DISABLED : THEME_TEXT);
    if (pressed) {
        OffsetRect(&rc, 1, 1);
    }
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ApplyCtlColor(HDC hdc, COLORREF bg, COLORREF fg = THEME_TEXT) {
    SetTextColor(hdc, fg);
    SetBkColor(hdc, bg);
}

void EnableDarkTitleBar(HWND hwnd) {
    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!dwmapi) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (setWindowAttribute) {
        const BOOL enabled = TRUE;
        // Windows 10 1809+ uses 20; older builds used 19 for the same dark-title-bar flag.
        setWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
        setWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
    }

    FreeLibrary(dwmapi);
}

void PopulateDefaults(HWND hwnd) {
    const RuntimeConfig cfg{};
    SetControlInt(hwnd, IDC_WIDTH, cfg.captureWidth);
    SetControlInt(hwnd, IDC_HEIGHT, cfg.captureHeight);
    SetControlInt(hwnd, IDC_TOLERANCE, cfg.tolerance);
    SetControlInt(hwnd, IDC_MIN_COLOR_PIXELS, cfg.minColorPixels);
    g_app.selectedTargetColor = cfg.targetColor;
    SetTargetColorText(hwnd, cfg.targetColor);
    SetControlKeyName(hwnd, IDC_KEY, cfg.targetKey);
    SetControlInt(hwnd, IDC_PRE_MIN, cfg.delayBeforePressMin);
    SetControlInt(hwnd, IDC_PRE_MAX, cfg.delayBeforePressMax);
    SetControlInt(hwnd, IDC_HOLD_MIN, cfg.keyHoldMin);
    SetControlInt(hwnd, IDC_HOLD_MAX, cfg.keyHoldMax);
    SetControlInt(hwnd, IDC_COOLDOWN_MIN, cfg.cooldownAfterPressMin);
    SetControlInt(hwnd, IDC_COOLDOWN_MAX, cfg.cooldownAfterPressMax);
    SetControlInt(hwnd, IDC_COOLDOWN_EVERY, cfg.cooldownAfterPressEvery);
    SetControlInt(hwnd, IDC_RELEASE_MIN, cfg.releaseDelayMin);
    SetControlInt(hwnd, IDC_RELEASE_MAX, cfg.releaseDelayMax);
    SetControlInt(hwnd, IDC_SCAN_MIN, cfg.scanIntervalMin);
    SetControlInt(hwnd, IDC_SCAN_MAX, cfg.scanIntervalMax);
    SetControlKeyName(hwnd, IDC_HELD_INPUT_KEY, cfg.heldInputKey);
    SetControlKeyName(hwnd, IDC_TOGGLE_HOTKEY, cfg.toggleHotkey);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SERIAL_PORT), cfg.serialPort.c_str());
    CheckDlgButton(hwnd, IDC_HOLD_WHILE_VISIBLE, cfg.holdWhileVisible ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_REQUIRE_HELD_INPUT, cfg.requireHeldInput ? BST_CHECKED : BST_UNCHECKED);
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
    g_app.start = CreateButton(hwnd, IDC_START, L"Start", 16, 16, 86, 30);
    g_app.stop = CreateButton(hwnd, IDC_STOP, L"Stop", 112, 16, 86, 30);
    g_app.apply = CreateButton(hwnd, IDC_APPLY, L"Apply", 208, 16, 86, 30);

    CreateLabel(hwnd, L"Status", 360, 21, 60, 20);
    g_app.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Disarmed",
                                   WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   430, 16, 140, 30, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), GetModuleHandleW(nullptr), nullptr);

    int y = 66;
    constexpr int labelW = 150;
    constexpr int editW = 90;
    constexpr int smallEditW = 70;
    constexpr int rowH = 24;
    constexpr int gap = 30;
    constexpr int leftLabelX = 16;
    constexpr int leftEditX = 190;
    constexpr int leftUnitX = 292;
    constexpr int rightLabelX = 420;
    constexpr int rightEditX = 560;
    constexpr int rightUnitX = 662;

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
    CreateLabel(hwnd, L"Required pixels", rightLabelX, y, labelW, rowH);
    HWND minPixels = CreateCombo(hwnd, IDC_MIN_COLOR_PIXELS, rightEditX, y - 3, editW, 160);
    PopulateHitThresholdChoices(minPixels);

    y += gap;
    CreateLabel(hwnd, L"Target color", leftLabelX, y, labelW, rowH);
    CreateButton(hwnd, IDC_PICK_COLOR, L"Pick screen", leftEditX, y - 3, editW, rowH);
    g_app.targetColorText = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                                            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                            leftEditX + 108, y - 3, 150, rowH,
                                            hwnd, reinterpret_cast<HMENU>(IDC_TARGET_COLOR), GetModuleHandleW(nullptr), nullptr);

    y += gap;
    CreateLabel(hwnd, L"Delay before key", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_PRE_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_PRE_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);
    CreateLabel(hwnd, L"Key to press", rightLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_KEY, rightEditX, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Key press length", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HOLD_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_HOLD_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);

    y += gap;
    CreateLabel(hwnd, L"Cooldown after key", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_COOLDOWN_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_COOLDOWN_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);
    CreateLabel(hwnd, L"Cooldown every", rightLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_COOLDOWN_EVERY, rightEditX, y - 3, editW, rowH);
    CreateLabel(hwnd, L"hits", rightUnitX, y, 48, rowH);

    y += gap;
    CreateWindowExW(0, L"BUTTON", L"Hold key while color is visible",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    leftLabelX, y - 2, 300, rowH,
                    hwnd, reinterpret_cast<HMENU>(IDC_HOLD_WHILE_VISIBLE), GetModuleHandleW(nullptr), nullptr);

    y += gap;
    CreateLabel(hwnd, L"Release delay", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_RELEASE_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_RELEASE_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);

    y += gap;
    CreateLabel(hwnd, L"Scan interval", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_SCAN_MIN, leftEditX, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"to", leftEditX + 82, y, 24, rowH);
    CreateEdit(hwnd, IDC_SCAN_MAX, leftEditX + 112, y - 3, smallEditW, rowH);
    CreateLabel(hwnd, L"ms", leftEditX + 194, y, 30, rowH);

    y += gap;
    CreateWindowExW(0, L"BUTTON", L"Only fire while held",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    leftLabelX, y - 2, 180, rowH,
                    hwnd, reinterpret_cast<HMENU>(IDC_REQUIRE_HELD_INPUT), GetModuleHandleW(nullptr), nullptr);
    CreateLabel(hwnd, L"Held input", rightLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_HELD_INPUT_KEY, rightEditX, y - 3, editW, rowH);

    y += gap;
    CreateLabel(hwnd, L"Start/Stop hotkey", leftLabelX, y, labelW, rowH);
    CreateEdit(hwnd, IDC_TOGGLE_HOTKEY, leftEditX, y - 3, editW, rowH);

    CreateLabel(hwnd, L"Arduino COM port", rightLabelX, y, labelW, rowH);
    HWND serialPort = CreateCombo(hwnd, IDC_SERIAL_PORT, rightEditX, y - 3, editW, 180);
    PopulateSerialPortChoices(serialPort);

    CreateLabel(hwnd, L"Live preview", 680, 16, 140, 20);
    g_app.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"minhanbotPreview", L"",
                                    WS_CHILD | WS_VISIBLE,
                                    680, 42, 200, 200, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.stats = CreateWindowExW(0, L"STATIC", L"Hits: 0  Closest: --",
                                  WS_CHILD | WS_VISIBLE,
                                  680, 250, 280, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

    PopulateDefaults(hwnd);
    EnableWindow(g_app.stop, FALSE);
}

void SetSelectedTargetColor(HWND hwnd, COLORREF color) {
    g_app.selectedTargetColor = {
        static_cast<int>(GetRValue(color)),
        static_cast<int>(GetGValue(color)),
        static_cast<int>(GetBValue(color))
    };
    SetTargetColorText(hwnd, g_app.selectedTargetColor);

    {
        std::lock_guard<std::mutex> lock(g_app.configMutex);
        g_app.config.targetColor = g_app.selectedTargetColor;
    }
}

void StartScreenColorPick(HWND hwnd) {
    if (g_app.colorPickActive.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    SetWindowTextW(GetDlgItem(hwnd, IDC_PICK_COLOR), L"Click screen...");

    std::thread([hwnd]() {
        while (!g_app.shuttingDown.load(std::memory_order_relaxed) &&
               (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
            Sleep(10);
        }

        while (!g_app.shuttingDown.load(std::memory_order_relaxed)) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                break;
            }

            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                POINT pt{};
                COLORREF color = CLR_INVALID;
                if (GetCursorPos(&pt)) {
                    HDC hdc = GetDC(nullptr);
                    if (hdc) {
                        color = GetPixel(hdc, pt.x, pt.y);
                        ReleaseDC(nullptr, hdc);
                    }
                }

                PostMessageW(hwnd, WM_APP_PICKED_COLOR, static_cast<WPARAM>(color), 0);
                return;
            }

            Sleep(10);
        }

        PostMessageW(hwnd, WM_APP_PICKED_COLOR, static_cast<WPARAM>(CLR_INVALID), 0);
    }).detach();
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
        EnableDarkTitleBar(hwnd);
        CreateMainControls(hwnd);
        ApplyConfig(hwnd);
        g_app.worker = std::thread(WorkerMain);
        return 0;

    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, ThemeBackgroundBrush());
        return 1;
    }

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
        case IDC_PICK_COLOR:
            StartScreenColorPick(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(child);
        if (id == IDC_STATUS || id == IDC_TARGET_COLOR) {
            ApplyCtlColor(hdc, THEME_PANEL);
            return reinterpret_cast<LRESULT>(ThemePanelBrush());
        }
        ApplyCtlColor(hdc, THEME_BG, THEME_MUTED);
        return reinterpret_cast<LRESULT>(ThemeBackgroundBrush());
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        ApplyCtlColor(hdc, THEME_FIELD);
        return reinterpret_cast<LRESULT>(ThemeFieldBrush());
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        ApplyCtlColor(hdc, THEME_FIELD_ALT);
        return reinterpret_cast<LRESULT>(ThemeListBrush());
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        ApplyCtlColor(hdc, THEME_BG);
        return reinterpret_cast<LRESULT>(ThemeBackgroundBrush());
    }

    case WM_DRAWITEM:
        DrawButtonControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

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
            L"Hits: %d/%d  Closest RGB: %d,%d,%d  +/- %d",
            g_app.lastHits.load(std::memory_order_relaxed),
            g_app.requiredHits.load(std::memory_order_relaxed),
            g_app.closestR.load(std::memory_order_relaxed),
            g_app.closestG.load(std::memory_order_relaxed),
            g_app.closestB.load(std::memory_order_relaxed),
            g_app.closestDelta.load(std::memory_order_relaxed));
        if (g_app.stats) {
            SetWindowTextW(g_app.stats, text);
        }
        return 0;
    }

    case WM_APP_PICKED_COLOR:
        g_app.colorPickActive.store(false, std::memory_order_relaxed);
        SetWindowTextW(GetDlgItem(hwnd, IDC_PICK_COLOR), L"Pick screen");
        if (static_cast<COLORREF>(wParam) != CLR_INVALID) {
            SetSelectedTargetColor(hwnd, static_cast<COLORREF>(wParam));
        }
        return 0;

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
    previewClass.lpszClassName = L"minhanbotPreview";
    previewClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&previewClass)) {
        return 1;
    }

    WNDCLASSW mainClass{};
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = hInstance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    mainClass.lpszClassName = L"minhanbotWindow";
    mainClass.hbrBackground = ThemeBackgroundBrush();
    if (!RegisterClassW(&mainClass)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        mainClass.lpszClassName,
        L"minhanbot",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        990, 450,
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
