cmake_minimum_required(VERSION 3.20)
project(Rhythm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Rhythm WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/AudioEngine.cpp
    src/SongClock.cpp
    src/ChartFile.cpp
    src/GameSession.cpp
    src/NoteLane.cpp
    src/Settings.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)
target_link_libraries(Rhythm PRIVATE xaudio2_9 ole32 comdlg32)

if(MINGW)
    target_link_options(Rhythm PRIVATE -municode)
endif()
