#include "core/crash.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
// windows.h has to come first.
#include <dbghelp.h>
#endif

namespace crash {
namespace {

#ifdef _WIN32
std::wstring g_directory;

// The process is in an unknown state here, so this touches nothing shared: no logger, no
// locks, no allocation past the path.
void writeDump(EXCEPTION_POINTERS* info) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_directory.c_str(), t.wYear,
               t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                           MiniDumpWithThreadInfo);
    BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                     info != nullptr ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    std::fwprintf(stderr,
                  written ? L"CapySR crashed; a dump is in %ls\n" : L"CapySR crashed; %ls failed\n",
                  path);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* info) {
    writeDump(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void onTerminate() {
    writeDump(nullptr);
    std::abort();
}
#endif

}  // namespace

void install(const std::string& directory) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    g_directory = std::filesystem::absolute(directory, ec).wstring();
    SetUnhandledExceptionFilter(onUnhandled);
    std::set_terminate(onTerminate);
#ifdef _MSC_VER
    // The dump says what happened; the abort() dialog would only hold the window open.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#else
    (void)directory;
#endif
}

}  // namespace crash
