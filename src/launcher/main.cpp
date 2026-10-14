#include <windows.h>
#include <shellapi.h>
#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failure(const std::wstring& message) {
#ifdef VE_CONSOLE_LAUNCHER
    const auto text = message + L"\r\n";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), size, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes.data(), DWORD(bytes.size()), &written, nullptr);
#else
    MessageBoxW(nullptr, message.c_str(), L"Visual Enhancer", MB_OK | MB_ICONERROR);
#endif
    return 1;
}

// Preserve the MSVC command-line contract, including embedded quotes, empty
// arguments, Unicode and trailing backslashes in quoted directory names.
std::wstring quoteArgument(const wchar_t* argument) {
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (const wchar_t* character = argument; *character; ++character) {
        if (*character == L'\\') { ++slashes; continue; }
        result.append(slashes * (*character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (*character == L'"') result += L'\\';
        result += *character;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

#ifdef VE_CONSOLE_LAUNCHER
BOOL WINAPI parentConsoleControl(DWORD event) {
    // The child shares the console and receives Ctrl+C/Ctrl+Break itself. Keep
    // its parent alive until cancellation has completed and an exit code exists.
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}
#endif

int launch(int count, wchar_t** arguments) {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
    if (!length || length >= std::size(executable)) return failure(L"Cannot locate the application folder.");
    const std::wstring location(executable, length);
    const auto separator = location.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return failure(L"Cannot locate the application folder.");
    const auto application = location.substr(0, separator + 1) + L"app\\" VE_LAUNCH_TARGET;
    std::wstring command = quoteArgument(application.c_str());
    for (int index = 1; index < count; ++index) command += L' ' + quoteArgument(arguments[index]);
    if (command.size() >= 32767) return failure(L"The application command line is too long.");
    // Windows applies this restriction to the child's initial dependency search.
    // Keep the caller's working directory so relative CLI inputs remain valid.
    if (!SetDllDirectoryW(L"")) return failure(L"Cannot configure application dependency loading.");

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    startup.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;
    DWORD flags = 0;
    BOOL inherit = FALSE;
#ifdef VE_CONSOLE_LAUNCHER
    std::array<HANDLE, 3> handles{};
    const auto closeHandles = [&] { for (const auto handle : handles) if (handle) CloseHandle(handle); };
    const std::array<DWORD, 3> identifiers{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    for (size_t index = 0; index < handles.size(); ++index) {
        const auto handle = GetStdHandle(identifiers[index]);
        if (handle && handle != INVALID_HANDLE_VALUE) {
            if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &handles[index], 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                closeHandles();
                return failure(L"Cannot forward a console input/output handle.");
            }
        } else {
            handles[index] = CreateFileW(L"NUL", index == 0 ? GENERIC_READ : GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handles[index] == INVALID_HANDLE_VALUE) {
                handles[index] = nullptr;
                closeHandles();
                return failure(L"Cannot configure console input/output.");
            }
        }
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributes(attributeSize);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize)) {
        closeHandles();
        return failure(L"Cannot configure console handle inheritance.");
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(), sizeof(handles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        closeHandles();
        return failure(L"Cannot configure console handle inheritance.");
    }
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    flags = EXTENDED_STARTUPINFO_PRESENT;
    inherit = TRUE;
    SetConsoleCtrlHandler(parentConsoleControl, TRUE);
#endif
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, inherit,
        flags, nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
#ifdef VE_CONSOLE_LAUNCHER
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    closeHandles();
#endif
    if (!started) return failure(L"Cannot start " + application + L". Keep the app folder beside this executable. Windows error " +
                                std::to_wstring(error) + L".");
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    const BOOL exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (!exited) return failure(L"Cannot read the application's exit status.");
    return static_cast<int>(exitCode);
}
} // namespace

#ifdef VE_CONSOLE_LAUNCHER
int wmain(int count, wchar_t** arguments) { return launch(count, arguments); }
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Explorer starts this windowless wrapper with startup cursor feedback.
    // GetMessage ends that feedback before we wait for the real GUI process.
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    if (PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0))
        GetMessageW(&message, nullptr, 0, 0);
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return failure(L"Cannot read the application arguments.");
    const int result = launch(count, arguments);
    LocalFree(arguments);
    return result;
}
#endif
