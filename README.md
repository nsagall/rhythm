# Rhythm
![Status](https://img.shields.io/badge/status-work--in--progress-yellow)

This is a simple game that I created with the express purpose of gaining familiarity with Claude Code.  It's a simple rhythm game for Win32, allowing a user to "play" various instruments and add them to the song in progress.  By default, the 4 note lanes correspond to the j,k,l, and ; keys (right-hand home keys on a qwerty keyboard), but they can be assigned to anything, including game controllers and MIDI devices, via the "assign inputs button".  There is an "Easy Mode" toggle that attempts to make the songs simpler easier to the play, but it's fully automated and doesn't always work great.

The songs that are not marked as "AI Slop" are all created by myself.  The AI Slop songs are directly from Claude without much direction, so they can be pretty painful to listen to.

There is an editor included to help make new tracks.  The Claude.md file has specifics about how the input formats work, but in general it works with pairs of .wav and midi files.  For the songs that I created, these files were all exported from Ableton Live.

## About the Code Quality

With some minor exceptions, all of the code was written by Claude.  I have started the process of going through it (I'm an experience C++ developer) and directing Claude to fixup places where I felt it did not do a good job.  For example, Claude seems to heavily favor plain data structs with all public members, so I've been slowly directing it to use a more object-oriented approach.  Claude has also produced a lot of duplicated code, so I've been directing it to used shared functions instead.  This is an ongoing process and it's mostly just begun - so there is still lots of ugly code there.

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
