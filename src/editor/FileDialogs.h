#pragma once

#include <windows.h>

#include <string>

// Thin wrappers over the Win32 Common Item Dialog COM interfaces
// (IFileOpenDialog/IFileSaveDialog). Deliberately never call
// CoInitializeEx/CoUninitialize themselves - EditorApp establishes an STA
// COM apartment once at startup (these dialogs require STA to behave
// correctly), before AudioEngine::Initialize() runs; see EditorApp.cpp for
// the ordering and why AudioEngine::Initialize()'s own COM init tolerates
// finding COM already set up this way.
namespace FileDialogs
{
    // Shows a folder picker (allows creating a new folder inline). Returns
    // false if the user cancels or the dialog couldn't be created.
    bool PickFolder(HWND owner, std::wstring& outFolderPath);

    // Shows a single-file open picker restricted to filterPattern (e.g.
    // L"*.chart" or L"*.wav"), labeled filterName in the dialog's type
    // dropdown. Returns false if the user cancels.
    bool PickOpenFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern, std::wstring& outFilePath);

    // Shows a save picker restricted to filterPattern, pre-filled with
    // defaultFileName (may be nullptr). Prompts before overwriting an
    // existing file. Returns false if the user cancels.
    bool PickSaveFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern,
                       const wchar_t* defaultFileName, std::wstring& outFilePath);
}
