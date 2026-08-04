cmake_minimum_required(VERSION 3.20)
project(Rhythm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored third-party MIDI file reader (craigsapp/midifile, BSD-2-Clause) -
# see third_party/midifile/LICENSE.txt. Kept as its own target so
# third-party code stays visibly separate from src/.
add_library(midifile STATIC
    third_party/midifile/src/Binasc.cpp
    third_party/midifile/src/MidiEvent.cpp
    third_party/midifile/src/MidiEventList.cpp
    third_party/midifile/src/MidiFile.cpp
    third_party/midifile/src/MidiMessage.cpp
)
target_include_directories(midifile PUBLIC third_party/midifile/include)

# UI-agnostic chart/audio core shared by the game (Rhythm) and the
# standalone chart editor (RhythmEditor) - see src/editor/. Kept as its own
# target so both executables always compile these files identically instead
# of risking their flags/definitions drifting apart.
add_library(RhythmCore STATIC
    src/ChartFile.cpp
    src/ChartMidi.cpp
    src/ChartTiming.cpp
    src/AudioEngine.cpp
    src/SongClock.cpp
)
target_include_directories(RhythmCore PUBLIC src)
target_compile_definitions(RhythmCore PUBLIC UNICODE _UNICODE)
target_link_libraries(RhythmCore PUBLIC midifile xaudio2_9 ole32 winmm)

add_executable(Rhythm WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/GameSession.cpp
    src/NoteLane.cpp
    src/NoteLaneModel.cpp
    src/NoteLaneGdiRenderer.cpp
    src/Settings.cpp
    src/SongLibrary.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)
target_link_libraries(Rhythm PRIVATE RhythmCore msimg32)

if(MINGW)
    target_link_options(Rhythm PRIVATE -municode)
endif()

# Vendored Dear ImGui (MIT license) - see third_party/imgui/LICENSE.txt.
# Only the pieces RhythmEditor actually uses: core + the Win32/DirectX11
# backend. imgui_demo.cpp is vendored for reference but intentionally not
# compiled in.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE)

add_executable(RhythmEditor WIN32
    src/editor/EditorMain.cpp
    src/editor/EditorApp.cpp
    src/editor/EditorChartIO.cpp
    src/editor/ClipPanel.cpp
    src/editor/BlockSchedule.cpp
    src/editor/BlockPlayer.cpp
    src/editor/BlockTimeline.cpp
    src/editor/BlockPropertiesPanel.cpp
    src/editor/FileDialogs.cpp
    src/editor/EditorSettings.cpp
)
target_include_directories(RhythmEditor PRIVATE src)
target_compile_definitions(RhythmEditor PRIVATE UNICODE _UNICODE)
target_link_libraries(RhythmEditor PRIVATE RhythmCore imgui d3d11 dxgi d3dcompiler ole32 uuid dwmapi)

if(MINGW)
    target_link_options(RhythmEditor PRIVATE -municode)
endif()
