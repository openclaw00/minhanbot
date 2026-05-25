# ColorZoneKey

Single-file Win32 C++ screen color monitor with a minimal native GUI.

## What it does

- Captures a small zone centered on the virtual desktop.
- Detects configured RGB target colors with per-channel tolerance.
- Sends a configurable keyboard press/release using `SendInput`.
- Provides runtime controls for capture size, tolerance, virtual key, timing ranges, cooldown, and capture interval.
- Shows a live zoomed preview of the monitored area.

## Build

From a Visual Studio Developer Command Prompt:

```bat
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE ColorZoneKey.cpp user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS
```

Cross-compile from macOS/Linux with MinGW-w64:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ ColorZoneKey.cpp -o ColorZoneKey.exe -luser32 -lgdi32
```

## Files

- `ColorZoneKey.cpp` - complete single-file Win32 source.
- `ColorZoneKey.exe` - Windows x64 GUI build.
