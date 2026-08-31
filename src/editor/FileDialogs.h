#pragma once

#include <windows.h>

#include <string>

// Thin wrappers over the Win32 Common Item Dialog COM interfaces. Never call
// CoInitializeEx/CoUninitialize themselves - EditorApp establishes an STA COM apartment at startup,
// which these dialogs require.
namespace FileDialogs
{
    // Shows a folder picker (allows creating a folder inline). Returns false if cancelled or the
    // dialog couldn't be created.
    bool PickFolder(HWND owner, std::wstring& outFolderPath);

    // Shows a single-file open picker restricted to filterPattern (e.g. L"*.chart"), labeled
    // filterName in the type dropdown. Returns false if cancelled.
    bool PickOpenFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern, std::wstring& outFilePath);

    // Shows a save picker restricted to filterPattern, pre-filled with defaultFileName (may be
    // nullptr). Prompts before overwriting. Returns false if cancelled.
    bool PickSaveFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern,
                       const wchar_t* defaultFileName, std::wstring& outFilePath);
}
