# Install wizard

Builds `RhythmSetup-<version>.exe` — a self-contained Inno Setup wizard that
installs **Rhythm.exe** (the game) and **RhythmEditor.exe** (the chart editor),
the whole `Content/` song library, `Colors.ini`, and every shared library the
two exes need. `ColorEditor.exe` is a dev-only tool and is not shipped.

## Shared libraries — what's covered

A fresh Windows 10 or 11 machine needs nothing else installed. Windows 7 SP1 and
8.1 are also supported. The install folder carries:

| Library | For | Why it's shipped |
|---|---|---|
| `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` | both apps | MinGW C++ runtime — never present on a stock machine. Copied from the compiler's `bin/`. |
| `xaudio2_9redist.dll` | both apps (audio) | XAudio 2.9 is in-box on Win 10/11 but not 7/8.1. `AudioEngine` loads the OS `xaudio2_9.dll` when present and falls back to this. Vendored in `third_party/xaudio2redist/`. |
| `d3dcompiler_47.dll` | RhythmEditor (ImGui/D3D11) | In-box on Win 10/11 and 8.1, but not 7. Copied from the build machine's `System32`. |

No Visual C++ Redistributable, DirectX End-User Runtime, or .NET is required —
this is a MinGW build and XAudio 2.9 needs no separate DX redist. For audio,
`AudioEngine` asks for `xaudio2_9.dll` by name first, so the OS copy is used on
Win 10/11 and the shipped `xaudio2_9redist.dll` only loads on 7/8.1. The shipped
`d3dcompiler_47.dll` sits next to `RhythmEditor.exe` and is used on every Windows
version (app directory precedence) — a fixed known-good D3DCompiler.

## Prerequisites

- A working Release build toolchain (CMake + Ninja + MinGW-w64), same as the game.
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (`winget install JRSoftware.InnoSetup`).
  The build script and the CMake `installer` target auto-detect `ISCC.exe` in
  `%ProgramFiles(x86)%`, `%ProgramFiles%`, and `%LOCALAPPDATA%\Programs`.

## Building the installer

Any one of these produces `build/installer/RhythmSetup-<version>.exe`:

```powershell
# 1. One-shot script (configure + build + stage + compile installer)
./installer/build-installer.ps1

# 2. CMake target (reuses your existing build/ directory)
cmake --build build --target installer

# 3. By hand
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix build/stage
& "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe" `
    /DAppVersion=1.0.0 /DStageDir=build\stage /DOutputDir=build\installer `
    installer/Rhythm.iss
```

`build-installer.ps1 -SkipBuild` re-stages and recompiles the installer without
rebuilding the exes.

## How it fits together

- **`CMakeLists.txt`** `install()` rules define the shipped layout: both exes at
  the install root with `Content/` and `Colors.ini` beside them (the game
  resolves both relative to its working directory — see `c_ContentRoot` in
  `MainWindow.cpp` and `ConfigFilePath()` in `ColorConfig.cpp`), plus the three
  MinGW runtime DLLs copied from the compiler's `bin/`.
- **`Rhythm.iss`** packages that staged tree. Version, stage path, and output
  path come in as `/D` defines so the same script serves the script, the CMake
  target, and manual runs.

## What the wizard does

- Per-user install by default (no admin prompt, into `%LOCALAPPDATA%\Programs\Rhythm`);
  the user can elevate from the first page to install machine-wide.
- Start Menu shortcuts for both apps (+ uninstaller); optional desktop shortcuts.
  Every shortcut sets **Start in** to the install folder, which is what lets the
  game find `Content/`.
- Offers to launch Rhythm on finish.
- Uninstaller removes everything it installed plus the `imgui.ini` the editor
  drops next to itself. Player data in `%APPDATA%\Rhythm` (settings, high scores)
  is deliberately left in place.

## Versioning

`build-installer.ps1` and the CMake target both read the version from
`project(Rhythm VERSION x.y.z ...)` in `CMakeLists.txt` — bump it there.
