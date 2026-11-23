#include "Common/Win32Util.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

namespace ge {

namespace {

// 日志锁用 CRITICAL_SECTION + INIT_ONCE，而不是 std::mutex：
// 这个 DLL 会被 FreeLibrary 卸载，函数内静态对象的析构顺序在卸载时不可控。
CRITICAL_SECTION g_logLock;
INIT_ONCE g_logOnce = INIT_ONCE_STATIC_INIT;

// 日志路径用定长缓冲而不是 std::wstring：卸载 DLL 时不必再析构一个堆对象。
wchar_t g_logPath[1024] = {};
bool g_logPathSet = false;

BOOL CALLBACK InitLogLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_logLock);
    return TRUE;
}

// 没显式指定时，退化为"本模块所在目录"。
std::wstring DefaultLogDirectory() {
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&DefaultLogDirectory), &module) == FALSE ||
        module == nullptr) {
        return {};
    }
    return GetDirectoryOf(GetModuleFilePath(module));
}

}  // namespace

std::wstring GetDirectoryOf(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return filePath.substr(0, slash + 1);
}

void SetLogDirectory(const std::wstring& directory) {
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path += L'\\';
    }
    path += L"GameEnhancement.log";
    ::wcsncpy_s(g_logPath, path.c_str(), _TRUNCATE);
    g_logPathSet = true;
}

std::wstring GetLogFilePath() {
    if (!g_logPathSet) {
        std::wstring directory = DefaultLogDirectory();
        if (directory.empty()) {
            wchar_t temp[MAX_PATH] = {};
            const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
            if (length != 0) {
                directory.assign(temp, length);
            }
        }
        SetLogDirectory(directory);
    }
    return std::wstring(g_logPath);
}

bool ReadMemory(uintptr_t address, void* buffer, size_t size) {
    if (address == 0 || buffer == nullptr || size == 0) {
        return false;
    }
    SIZE_T read = 0;
    const BOOL ok = ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                                        buffer, size, &read);
    return ok != FALSE && read == size;
}

bool WriteMemory(uintptr_t address, const void* buffer, size_t size) {
    if (address == 0 || buffer == nullptr || size == 0) {
        return false;
    }

    // 目标页可能是只读的（例如写进 .data 里的常量或受保护的堆页），
    // 先放开再写，写完还原成原来的保护属性。
    DWORD oldProtect = 0;
    const bool flipped = ::VirtualProtect(reinterpret_cast<LPVOID>(address), size,
                                          PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;

    SIZE_T written = 0;
    const BOOL ok = ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPVOID>(address),
                                         buffer, size, &written);

    if (flipped) {
        DWORD ignored = 0;
        ::VirtualProtect(reinterpret_cast<LPVOID>(address), size, oldProtect, &ignored);
    }

    return ok != FALSE && written == size;
}

bool IsUserPointer(uintptr_t value) {
    // 0x10000 以下留给 NULL 页与系统结构；0x7FFF_FFFF_FFFF 以上是内核空间。
    constexpr uintptr_t kMinUser = 0x10000;
    constexpr uintptr_t kMaxUser = 0x00007FFFFFFEFFFFull;
    return value >= kMinUser && value <= kMaxUser && (value % alignof(void*)) == 0;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

void LogLine(const char* format, ...) {
    ::InitOnceExecuteOnce(&g_logOnce, InitLogLock, nullptr, nullptr);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    char body[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
    va_end(args);

    char line[1200] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u][tid %lu] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                static_cast<unsigned long>(::GetCurrentThreadId()), body);

    ::OutputDebugStringA(line);

    ::EnterCriticalSection(&g_logLock);
    const std::wstring path = GetLogFilePath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") == 0 && file != nullptr) {
        std::fwrite(line, 1, std::strlen(line), file);
        std::fclose(file);
    }
    ::LeaveCriticalSection(&g_logLock);
}

std::wstring GetModuleFilePath(HMODULE module) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return {};
    }
    return std::wstring(buffer, length);
}

}  // namespace ge
