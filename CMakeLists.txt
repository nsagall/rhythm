cmake_minimum_required(VERSION 3.20)
project(Rhythm VERSION 1.0.0 LANGUAGES CXX)

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
    src/ChartSong.cpp
    src/ChartClip.cpp
    src/ChartMidi.cpp
    src/AudioEngine.cpp
    src/SongClock.cpp
)
target_include_directories(RhythmCore PUBLIC src)
target_compile_definitions(RhythmCore PUBLIC UNICODE _UNICODE)
# No xaudio2_9 here on purpose: AudioEngine resolves XAudio2Create at runtime
# (LoadLibrary xaudio2_9.dll, else the bundled xaudio2_9redist.dll) so the
# build has no load-time dependency on a DLL that is missing on Windows 7/8.1.
target_link_libraries(RhythmCore PUBLIC midifile ole32 winmm)

# Colors.ini load/save for Colors.h's ClipColor::/GameColors:: globals -
# its own tiny target (no midifile/xaudio2/audio deps at all) so
# ColorEditor.exe can link just this instead of the much heavier
# RhythmCore, while Rhythm still gets the exact same code the editor
# writes with, no drift.
add_library(RhythmColors STATIC
    src/ColorConfig.cpp
)
target_include_directories(RhythmColors PUBLIC src)
target_compile_definitions(RhythmColors PUBLIC UNICODE _UNICODE)

add_executable(Rhythm WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/GameSession.cpp
    src/SectionInstance.cpp
    src/StreakTracker.cpp
    src/NoteLane.cpp
    src/NoteLaneModel.cpp
    src/NoteLaneGdiRenderer.cpp
    src/Settings.cpp
    src/SongLibrary.cpp
    src/LaneBindings.cpp
    src/MidiInputManager.cpp
    src/GamepadInputManager.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)
target_link_libraries(Rhythm PRIVATE RhythmCore RhythmColors msimg32 xinput9_1_0)

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
    src/editor/SongPropertiesPanel.cpp
    src/editor/PaneSplitter.cpp
)
target_include_directories(RhythmEditor PRIVATE src)
target_compile_definitions(RhythmEditor PRIVATE UNICODE _UNICODE)
target_link_libraries(RhythmEditor PRIVATE RhythmCore imgui d3d11 dxgi d3dcompiler ole32 uuid dwmapi)

if(MINGW)
    target_link_options(RhythmEditor PRIVATE -municode)
endif()

# Standalone Colors.ini tuning tool - lets an author see every game color
# as a swatch (labeled with the same name Colors.ini/ColorConfig.cpp use)
# and repaint it with an ImGui hue-wheel picker, side by side, without
# touching game code. Reuses the same vendored ImGui + Win32/DX11 backend
# as RhythmEditor, but links RhythmColors (not RhythmCore) - it has no
# reason to touch chart/audio code at all.
add_executable(ColorEditor WIN32
    src/coloreditor/ColorEditorMain.cpp
)
target_include_directories(ColorEditor PRIVATE src)
target_compile_definitions(ColorEditor PRIVATE UNICODE _UNICODE)
target_link_libraries(ColorEditor PRIVATE RhythmColors imgui d3d11 dxgi d3dcompiler dwmapi)

if(MINGW)
    target_link_options(ColorEditor PRIVATE -municode)
endif()

# ---------------------------------------------------------------------------
# Install / packaging layout for the Inno Setup wizard (installer/Rhythm.iss)
# ---------------------------------------------------------------------------
# `cmake --install <builddir> --prefix <stage>` produces exactly the tree the
# installer ships: the two shipping executables at the root, the whole Content/
# song library and Colors.ini beside them (both resolved relative to the
# process working directory at runtime - see c_ContentRoot in MainWindow.cpp
# and ConfigFilePath() in ColorConfig.cpp), plus the MinGW runtime DLLs the
# exes link against and the two Microsoft redistributables that let the apps
# also run on Windows 7 / 8.1 (see below). ColorEditor is a dev-only tool and
# is deliberately left out. installer/build-installer.ps1 drives configure ->
# build -> install -> ISCC end to end.
install(TARGETS Rhythm RhythmEditor RUNTIME DESTINATION ".")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Content" DESTINATION ".")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Colors.ini" DESTINATION ".")

# XAudio 2.9 (both apps) and the D3DCompiler (RhythmEditor's ImGui backend) are
# in-box on Windows 10/11 but absent on Windows 7 / 8.1. Ship both beside the
# exes so those systems are covered - the app directory is searched before
# System32, so on 10/11 the OS copies still win and these are just dead weight.
#   - xaudio2_9redist.dll: vendored (third_party/xaudio2redist/), loaded by
#     name at runtime as AudioEngine's fallback - see its CreateXAudio2Engine.
#   - d3dcompiler_47.dll: copied from this build machine's System32 (it is a
#     Microsoft-redistributable component; every Win8.1+ box building this app
#     has it). RhythmEditor imports it at load time.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xaudio2redist/xaudio2_9redist.dll"
        DESTINATION ".")

file(TO_CMAKE_PATH "$ENV{SystemRoot}" _windir)
set(_d3dcompiler "${_windir}/System32/d3dcompiler_47.dll")
if(EXISTS "${_d3dcompiler}")
    install(FILES "${_d3dcompiler}" DESTINATION ".")
else()
    message(WARNING "d3dcompiler_47.dll not found at ${_d3dcompiler}; "
                    "RhythmEditor will not start on Windows 7/8.1 without it")
endif()

if(MINGW)
    # libstdc++ / libgcc / libwinpthread are not present on a stock Windows
    # box; copy them from the compiler's own bin/ next to the exes. objdump -p
    # on both exes confirms these three are the only non-system imports.
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${_mingw_bin}/${_dll}")
            install(FILES "${_mingw_bin}/${_dll}" DESTINATION ".")
        else()
            message(WARNING "Runtime DLL not found for packaging: ${_mingw_bin}/${_dll}")
        endif()
    endforeach()
elseif(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
    include(InstallRequiredSystemLibraries)
    install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ".")
endif()

# Optional convenience target: `cmake --build <builddir> --target installer`
# builds the setup .exe in one step, if Inno Setup's compiler is discoverable.
find_program(ISCC_EXECUTABLE
    NAMES ISCC iscc
    PATHS
        "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
        "$ENV{ProgramFiles}/Inno Setup 6"
        "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    DOC "Inno Setup command-line compiler (ISCC.exe)")

if(ISCC_EXECUTABLE)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/stage" _stage_native)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/installer" _installer_native)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/installer/Rhythm.iss" _iss_native)
    add_custom_target(installer
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${CMAKE_BINARY_DIR}/stage"
        COMMAND "${ISCC_EXECUTABLE}"
                "/DAppVersion=${PROJECT_VERSION}"
                "/DStageDir=${_stage_native}"
                "/DOutputDir=${_installer_native}"
                "${_iss_native}"
        DEPENDS Rhythm RhythmEditor
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Staging install tree and building RhythmSetup-${PROJECT_VERSION}.exe with Inno Setup"
        VERBATIM USES_TERMINAL)
endif()
