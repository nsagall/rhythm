#pragma once

#include <string>

// Pure wide-string helpers shared by the chart parser (ChartSong.cpp) and the editor's chart I/O
// (editor/EditorChartIO.cpp).
namespace ChartTextUtil
{

// Trims leading/trailing whitespace from a wide string.
inline std::wstring Trim(const std::wstring& s)
{
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos)
    {
        return L"";
    }
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Returns the directory portion of a path (everything up to and including the last slash), or "" if none.
inline std::wstring GetDirectory(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L"";
    }
    return path.substr(0, slash + 1);
}

} // namespace ChartTextUtil
