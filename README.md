# Rhythm

A Windows C++ desktop application.

## Building

Requires CMake and a C++20 compiler (MinGW-w64 g++ or MSVC).

```powershell
cmake -B build -G Ninja
cmake --build build
```

## Running

```powershell
.\build\Rhythm.exe
```

## Installer

`installer/build-installer.ps1` packages the game and the chart editor into a
single `RhythmSetup-<version>.exe` wizard (Inno Setup). See
[`installer/README.md`](installer/README.md).
