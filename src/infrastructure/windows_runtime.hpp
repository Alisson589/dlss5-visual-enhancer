#pragma once
#include <windows.h>
#include <stdexcept>
#include <iterator>
#include <string>

namespace ve {
inline std::wstring windowsSystemLibrary(const wchar_t* name) {
    wchar_t directory[32768]{};
    const auto length = GetSystemDirectoryW(directory, DWORD(std::size(directory)));
    if (!length || length >= std::size(directory))
        throw std::runtime_error("Cannot locate Windows system libraries.");
    return std::wstring(directory, length) + L'\\' + name;
}
inline void configureWindowsRuntime() {
    // Dependencies live beside the real application executable. Later plugin
    // loads must not consult the caller's working directory or PATH.
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
        throw std::runtime_error("Cannot configure secure application dependency loading.");
}
} // namespace ve
