#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Runtime-tunable backing store for every color in Colors.h (ClipColor::/
// GameColors::) - those live as mutable `inline COLORREF` globals rather
// than `constexpr` specifically so this file's LoadFromIni can overwrite
// them in place before anything draws with them, and so ColorEditor.exe
// (src/coloreditor/) can edit + persist the same values the game itself
// reads, with no separate copy to keep in sync.
namespace Colors
{

// One named, editable color - section/name are how both Colors.ini and
// ColorEditor identify it; value points straight at the live global
// (ClipColor::c_Neutral, GameColors::c_BgTop, ...) the game actually draws
// with. section/name are plain ASCII string literals owned by
// AllEntries()'s static table, so they outlive any caller.
struct Entry
{
    const char* section;
    const char* name;
    COLORREF* value;
};

// Every tunable color, in a fixed display/save order - built once, shared
// by LoadFromIni/SaveToIni and by ColorEditor's own UI, so the two can
// never disagree about what's editable.
const std::vector<Entry>& AllEntries();

// Colors.ini lives next to Content/ (see c_ContentRoot in MainWindow.cpp),
// relative to the process's working directory - not under %APPDATA% like
// Settings, since this is authoring-time tuning data meant to be checked
// into the repo and shared, not a per-player preference. Both Rhythm.exe
// and ColorEditor.exe must be run from the repo root for this path to
// resolve, same convention as "Content/...".
std::wstring ConfigFilePath();

// Overwrites each entry whose key is present in Colors.ini; an entry with
// no saved key (or no file at all) is left at Colors.h's compiled-in
// value. Safe to call unconditionally at startup.
void LoadFromIni();

// Writes every entry's current value to Colors.ini, creating the file if
// needed. Called by ColorEditor's Save button.
void SaveToIni();

} // namespace Colors
