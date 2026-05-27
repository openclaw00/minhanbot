# minhanbot

Single-file Win32 C++ screen color monitor with a minimal native GUI.

## What it does

- Captures a small zone centered on the virtual desktop.
- Detects configured RGB target colors with per-channel tolerance.
- Sends a configurable keyboard press/release using `SendInput`.
- Provides runtime controls for capture size, tolerance, key to press, delay before key, key press length, hold-while-visible mode, capture interval, and a Start/Stop hotkey.
- Shows a live zoomed preview of the monitored area.

## Build

From a Visual Studio Developer Command Prompt:

```bat
rc ColorZoneKey.rc
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /Fe:minhanbot.exe ColorZoneKey.cpp ColorZoneKey.res user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS
```

Cross-compile from macOS/Linux with MinGW-w64:

```sh
x86_64-w64-mingw32-windres ColorZoneKey.rc -O coff -o ColorZoneKey.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ ColorZoneKey.cpp ColorZoneKey.res -o minhanbot.exe -luser32 -lgdi32
```

## Files

- `ColorZoneKey.cpp` - complete single-file Win32 source.
- `ColorZoneKey.rc` - Windows icon resource.
- `assets/app-logo.png` and `assets/app.ico` - app logo and executable icon.
- `minhanbot.exe` - Windows x64 GUI build.
