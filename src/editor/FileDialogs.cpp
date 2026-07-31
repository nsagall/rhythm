#include "FileDialogs.h"

#include <shobjidl.h>

namespace
{

// Shared Show()/GetResult() sequence for both dialog kinds - IFileSaveDialog
// publicly derives from IFileDialog, so a raw IFileSaveDialog* upcasts
// implicitly when passed here.
bool ShowDialog(IFileDialog* dialog, HWND owner, std::wstring& outPath)
{
    if (FAILED(dialog->Show(owner)))
    {
        // Cancelled, or failed - either way, nothing to report.
        return false;
    }

    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr)
    {
        return false;
    }

    PWSTR path = nullptr;
    bool ok = false;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
    {
        outPath = path;
        ok = true;
    }
    if (path != nullptr)
    {
        CoTaskMemFree(path);
    }
    item->Release();
    return ok;
}

} // namespace

namespace FileDialogs
{

bool PickFolder(HWND owner, std::wstring& outFolderPath)
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr)
    {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

    bool ok = ShowDialog(dialog, owner, outFolderPath);
    dialog->Release();
    return ok;
}

bool PickOpenFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern, std::wstring& outFilePath)
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr)
    {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);

    COMDLG_FILTERSPEC filter[] = {{filterName, filterPattern}};
    dialog->SetFileTypes(1, filter);
    dialog->SetFileTypeIndex(1);

    bool ok = ShowDialog(dialog, owner, outFilePath);
    dialog->Release();
    return ok;
}

bool PickSaveFile(HWND owner, const wchar_t* filterName, const wchar_t* filterPattern,
                   const wchar_t* defaultFileName, std::wstring& outFilePath)
{
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr)
    {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

    COMDLG_FILTERSPEC filter[] = {{filterName, filterPattern}};
    dialog->SetFileTypes(1, filter);
    dialog->SetFileTypeIndex(1);
    if (defaultFileName != nullptr)
    {
        dialog->SetFileName(defaultFileName);
    }

    bool ok = ShowDialog(dialog, owner, outFilePath);
    dialog->Release();
    return ok;
}

} // namespace FileDialogs
