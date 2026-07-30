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

add_executable(Rhythm WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/AudioEngine.cpp
    src/SongClock.cpp
    src/ChartFile.cpp
    src/ChartMidi.cpp
    src/GameSession.cpp
    src/NoteLane.cpp
    src/Settings.cpp
    src/SongLibrary.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)
target_link_libraries(Rhythm PRIVATE midifile xaudio2_9 ole32 msimg32 winmm)

if(MINGW)
    target_link_options(Rhythm PRIVATE -municode)
endif()
