#pragma once

#include <windows.h>

#include <string>

// Dear ImGui's text widgets speak UTF-8 exclusively; the rest of this
// codebase (chart parsing, file paths, Win32 wide APIs) speaks std::wstring
// throughout. These are the only two conversion points the editor needs.
inline std::string ToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), len, nullptr, nullptr);
    return result;
}

inline std::wstring FromUtf8(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), len);
    return result;
}
