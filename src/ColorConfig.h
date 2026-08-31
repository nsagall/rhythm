#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Runtime-tunable backing store for every color in Colors.h. Those are mutable `inline COLORREF`
// globals so LoadFromIni can overwrite them in place before anything draws, and ColorEditor.exe can
// edit and persist the same values the game reads.
namespace Colors
{

// One named, editable color. section/name identify it in Colors.ini and ColorEditor; value points
// at the live global the game draws with. section/name are ASCII literals owned by AllEntries()'s
// static table.
struct Entry
{
    const char* section;
    const char* name;
    COLORREF* value;
};

// Every tunable color, in a fixed display/save order - shared by LoadFromIni/SaveToIni and
// ColorEditor's UI.
const std::vector<Entry>& AllEntries();

// Colors.ini lives next to Content/, relative to the working directory - not under %APPDATA% like
// Settings, since this is authoring-time tuning data meant to be checked in. Both executables must
// be run from the repo root.
std::wstring ConfigFilePath();

// Overwrites each entry whose key is present in Colors.ini; others stay at Colors.h's compiled-in
// value. Safe to call unconditionally at startup.
void LoadFromIni();

// Writes every entry's current value to Colors.ini, creating the file if needed.
void SaveToIni();

} // namespace Colors
