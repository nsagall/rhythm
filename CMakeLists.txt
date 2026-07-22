cmake_minimum_required(VERSION 3.20)
project(Rhythm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Rhythm WIN32
    src/main.cpp
)

target_compile_definitions(Rhythm PRIVATE UNICODE _UNICODE)

if(MINGW)
    target_link_options(Rhythm PRIVATE -municode)
endif()
