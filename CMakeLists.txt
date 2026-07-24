cmake_minimum_required(VERSION 3.20)
project(Rhythm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(Rhythm WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/AudioPlayer.cpp
    src/Settings.cpp
    src/BeatScroller.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)
target_link_libraries(Rhythm PRIVATE winmm comdlg32 Threads::Threads)

if(MINGW)
    target_link_options(Rhythm PRIVATE -municode)
endif()
