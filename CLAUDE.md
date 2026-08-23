# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Rhythm is a Windows C++20 desktop rhythm game, plus `RhythmEditor`, a standalone
chart-authoring tool for it. A "chart" (`.chart` text file + `.wav` stem files +
`.mid` pattern files, all in one folder under `Content/`) defines a song: a pool of
reusable audio+MIDI clips and an ordered sequence of gameplay sections built from
them. The player presses `kLaneCount` (4) lanes in time with notes scrolling down
the screen, judged against the clip's MIDI-authored pattern.

## Building

Requires CMake and a C++20 compiler (MinGW-w64 g++ or MSVC). No test framework or
CI is configured — this is a personal/hobby project built via the VS Code tasks
below or directly with CMake.

```powershell
cmake -B build -G Ninja
cmake --build build
```

The VS Code tasks (`.vscode/tasks.json`) configure/build into `build/debug` with
`-DCMAKE_BUILD_TYPE=Debug` instead; `.vscode/launch.json` debugs `Rhythm.exe` via
gdb. Two executables come out of the build: `Rhythm.exe` (the game) and
`RhythmEditor.exe` (the chart editor). Both are `WIN32` (GUI) subsystem apps.

There is no `test`/`ctest` target — see **Diagnostics instead of a test suite** below.

## Architecture

### Two executables, one shared core

`CMakeLists.txt` defines two static libraries and two executables:

- **`RhythmCore`** — UI-agnostic chart/audio core shared by both executables:
  `ChartFile`/`ChartMidi` (parsing; `ChartClip` also carries the pure timing
  math, see below), `AudioEngine` (XAudio2 wrapper), `SongClock`. Kept as its
  own CMake target specifically so both executables always compile these files
  with identical flags/definitions rather than risking drift.
- **`Rhythm`** — the game: `MainWindow` (Win32/GDI), `GameSession` (live judging
  state machine), `SectionInstance`, `NoteLane`/`NoteLaneModel`/
  `NoteLaneGdiRenderer` (rendering), `SongLibrary`, `Settings`.
- **`RhythmEditor`** — the chart editor: Dear ImGui (vendored in
  `third_party/imgui`, Win32 + DirectX11 backend) driving `EditorApp` and its
  panels under `src/editor/`.
- **`midifile`** — vendored `craigsapp/midifile` (BSD-2-Clause), wrapped by
  `ChartMidi`.

`ChartClip`'s own timing methods (declared alongside its data in `src/ChartFile.h`,
implemented in `src/ChartTiming.cpp`) are the crux of the shared-core design:
all note-onset and loop/advance-timing arithmetic is pure functions of their
parameters plus `*this` (no wall-clock or other instance state) — non-static
methods for anything that reads a single clip's own data (`NextOnsetAfter`,
`ComputeClipPhaseSeconds`, ...), static methods for everything else
(`ComputeLearnAdvanceSeconds`, `ValidateArrangementAlignment`, ...), used
identically by the live game (`GameSession`) and the editor's analytical
scheduler (`src/editor/BlockSchedule.h`) — so the two can never compute
different answers for the same chart. `BlockSchedule::Build`
precomputes an entire song's timeline assuming a perfect player, then
`BlockSchedule::Seek` answers "what's true at second X" as a pure function —
used to scrub the editor's timeline without actually playing anything forward.

### The chart format and clip/section model

A chart has a `[song]` header (title/bpm/time_signature/default tolerances), a
pool of `[clip]` blocks (a stem `.wav` + optional `.mid` pattern + judging
thresholds — reusable, and referenced by name from multiple sections), and an
ordered list of section blocks that actually drive gameplay:

- `[learn]` — judge presses/releases against the clip's pattern; locks in after
  `hits_required` correct hits in a row (or, in `learn_mode = dont_fail`,
  reversibly tracks passing/failing).
- `[break]` — stop everything else, play this clip for `loop_count` loops, no
  judging.
- `[reset]` — stop everything else and advance immediately; no clip.
- `[background]` — queue this clip to start (without stopping anything) when the
  *next* section begins, then loop indefinitely.

`ChartFile::Load` (`src/ChartFile.h/.cpp`) is the *only* way a `ChartSong` is
constructed — it parses and fully validates the text format plus the referenced
`.wav`/`.mid` files. `ChartMidi::LoadLaneNotes` extracts notes for the 4 fixed
lane pitches (`kLaneMidiPitches` in `src/LaneConfig.h`) from a clip's MIDI file;
`kLaneCount`/`kLaneMidiPitches`/`kNoteFallBeats` in `LaneConfig.h` are the single
source of truth other lane-shaped code sizes itself off.

Every clip's loop boundaries are anchored to a shared **arrangement origin** —
the wall-clock beat/second the first clip of an unbroken run of
continuously-sounding clips began at (`GameSession::m_arrangementOriginSeconds`,
`BlockSchedule::Build`'s own local equivalent) — not absolute beat/second 0, and
not a separate origin per clip. This rests on three chart-authoring assumptions,
checked once at load time by `ChartClip::ValidateArrangementAlignment` (never
at runtime, so a validated chart can't fail mid-song): every clip's length is a
whole number of bars; clips that ever sound concurrently are the same length or
whole multiples of each other; and a clip only ever joins an arrangement already
in progress on one of its own bar boundaries — restricted, for whatever joins
right after a Learn section specifically, to "evenly divides that Learn clip's
own length," since a real player's loop count there is unbounded and unknowable
at load time (see the function's own doc comment for why). This is what lets a
clip's very first onset use the exact same formula (`ChartClip::
NextOnsetAfter`) as a later reuse, with no separate "fresh start" case anywhere.
Read `ChartClip`'s timing-method doc comments (`src/ChartFile.h`) before
touching any of this timing code, they explain the *why* in detail and are
treated as the canonical spec.

### Editor's document model is deliberately separate from the runtime model

`src/editor/EditorDocument.h` (`EditorDocument`/`EditorClip`/`EditorBlock`) is a
distinct type from `ChartFile.h`'s `ChartSong`/`ChartClip`/`ChartSection` — the
runtime structs are produced by parsing an already-valid chart and throw away
information the editor needs while a chart is still being assembled (whether a
tolerance was explicitly overridden vs. inherited, stable IDs that survive
reordering/renaming). The editor's UI calls a section a "block" (see
`BlockTimeline`), but the underlying `.chart` file format and runtime code always
say "section" — that vocabulary split is intentional, not inconsistent naming.
`EditorChartIO` re-validates by round-tripping through the real `ChartFile::Load`,
so the editor and the live game never disagree about what's valid.

### Renderer/model separation

`NoteLaneModel` (game logic: which notes are visible, their judged state) and
`NoteLaneGdiRenderer` (GDI drawing) communicate only through `NoteLaneScene.h`'s
plain data structs (`NoteLaneScene`, `SceneNote`, `ClipInstance`) — beats, lane
indices, and semantic state only, no pixel positions or HDC. `INoteLaneRenderer`
(`NoteLaneRenderer.h`) is the swap point for a different visual style with zero
changes to the model. Follow this pattern (data-only scene structs + an interface)
if adding a new renderer or porting the game off GDI.

### Diagnostics instead of a test suite

There is no automated test framework. Verification happens through standalone
`*DiagMain.cpp` files in `src/` (e.g. `ChartValidationDiagMain.cpp`,
`MelodicaDiagMain.cpp`, `RepeatUntilLockedInDiagMain.cpp`,
`EasyModeGraceDiagMain.cpp`) — each is a `main()` that headlessly drives
`GameSession`/`ChartFile`/`ChartMidi` against real or fixture charts
(`test_charts/`, including deliberately-`broken_*.chart` files meant to fail
validation) and prints results for manual inspection. **None of these are wired
into `CMakeLists.txt`** — compile one directly against `RhythmCore` when you need
it, e.g.:

```powershell
g++ -std=c++20 -Isrc -Ithird_party/midifile/include src/ChartValidationDiagMain.cpp src/ChartFile.cpp src/ChartMidi.cpp src/ChartTiming.cpp third_party/midifile/src/*.cpp -o diag.exe
```

When adding a diagnostic for new behavior, follow the existing files' pattern:
a top-of-file comment stating what it verifies and why, then a headless
`GameSession`-driving `main()`.

`tools/SongGenerator.cpp` is a separate, standalone synth + MIDI writer for
generating a chart's `.wav`/`.mid` pairs from code (deterministic — seeded LCG
noise) instead of real recordings. It also has zero dependency on the engine and
isn't in `CMakeLists.txt`:

```powershell
g++ -O2 -std=c++20 -o song_gen.exe tools/SongGenerator.cpp
./song_gen.exe   # run from repo root; paths are relative to it
```

To author a new generated song, keep everything above `main()` and replace only
`main()`'s note-event tables/clip list.

## Working conventions specific to this codebase

- Header comments in this codebase are long and treated as the authoritative
  spec for *why*, not just *what* — e.g. `ChartFile.h` (including `ChartClip`'s
  own timing methods), `GameSession.h` explain subtle invariants (phase origins, Pass vs. DontFail
  lock-in semantics, count-in catch-up) that aren't obvious from the code alone.
  Read the relevant header comment before modifying timing/judging logic, and
  keep new code held to the same standard when the invariant is genuinely
  non-obvious.
- `RhythmCore` files must stay UI-agnostic (no HWND/HDC/input-device knowledge) —
  that's what lets `GameSession`/`ChartClip`'s timing methods be shared verbatim
  between the game and the editor's analytical scheduler.
- Ableton Live (the DAW used to author charts) numbers octaves one lower than the
  "middle C = C4" convention — `kLaneMidiPitches` in `LaneConfig.h` is chosen to
  match what Ableton's piano roll displays, not the general MIDI convention.
