# minhanbot

Single-file Win32 C++ screen color monitor with a minimal native GUI.

## What it does

- Captures a small zone centered on the virtual desktop through DXGI Desktop Duplication.
- Detects configured RGB target colors with per-channel tolerance and a configurable required pixel count.
- Sends a configurable keyboard press/release using `SendInput`.
- Optionally sends key down/up commands to an external USB HID bridge over serial, so the target app sees input from a real keyboard device.
- Provides runtime controls for capture size, tolerance, required color pixels, key to press, bell-curve reaction delay, key press length, hold-while-visible mode, release delay, capture interval, and a Start/Stop hotkey.
- Shows a live zoomed preview of the monitored area.

## External USB HID input

Use an Arduino Leonardo, Arduino Micro, Pro Micro, or another ATmega32u4 board that supports the Arduino `Keyboard` library.

1. Open `firmware/serial-hid-bridge/serial-hid-bridge.ino` in the Arduino IDE.
2. Select the board and upload the sketch.
3. Plug the board into the Windows machine running `minhanbot.exe`.
4. Check Device Manager for the board's COM port, for example `COM4`.
5. In minhanbot, check `External USB HID`, enter the COM port, then press `Apply`.

The desktop app still does detection and timing. The board receives commands such as `D 4A` and `U 4A`, then performs the actual USB keyboard press/release.

## Build

From a Visual Studio Developer Command Prompt:

```bat
rc minhanbot.rc
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /Fe:minhanbot.exe minhanbot.cpp minhanbot.res user32.lib gdi32.lib d3d11.lib dxgi.lib /link /SUBSYSTEM:WINDOWS
```

Cross-compile from macOS/Linux with MinGW-w64:

```sh
x86_64-w64-mingw32-windres minhanbot.rc -O coff -o minhanbot.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ minhanbot.cpp minhanbot.res -o minhanbot.exe -luser32 -lgdi32 -ld3d11 -ldxgi
```

## Files

- `minhanbot.cpp` - complete single-file Win32 source.
- `minhanbot.rc` - Windows icon resource.
- `firmware/serial-hid-bridge/serial-hid-bridge.ino` - optional external USB HID bridge firmware.
- `assets/app-logo.png` and `assets/app.ico` - app logo and executable icon.
- `minhanbot.exe` - Windows x64 GUI build.
