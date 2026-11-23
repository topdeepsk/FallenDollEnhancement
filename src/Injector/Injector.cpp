#include "Injector/Injector.h"

#include <cwchar>
#include <iterator>
#include <vector>

#include <tlhelp32.h>

#include "Injector/PeExport.h"

namespace ge {

namespace {

constexpr DWORD kOpenProcessRights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ | SYNCHRONIZE;

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

bool EndsWithNoCase(const std::wstring& text, const std::wstring& suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    return ToLower(text.substr(text.size() - suffix.size())) == ToLower(suffix);
}

uintptr_t FindModuleByName(DWORD pid, const std::wstring& moduleName, std::wstring& fullPath) {
    fullPath.clear();

    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    uintptr_t base = 0;
    if (::Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (ToLower(entry.szModule) == ToLower(moduleName) ||
                EndsWithNoCase(entry.szExePath, moduleName)) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                fullPath = entry.szExePath;
                break;
            }
        } while (::Module32NextW(snapshot, &entry) != FALSE);
    }

    ::CloseHandle(snapshot);
    return base;
}

// 取目标进程里 kernel32 的某个导出地址。
//
// 不能直接把自己的 GetProcAddress 结果当成目标地址：ASLR 下两个进程的
// kernel32 基址通常不同。用"本地偏移不变"这一点做换算。
uintptr_t RemoteKernel32Proc(DWORD pid, const char* procName) {
    const HMODULE localKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (localKernel32 == nullptr) {
        return 0;
    }
    const auto localProc = reinterpret_cast<uintptr_t>(
        ::GetProcAddress(localKernel32, procName));
    if (localProc == 0) {
        return 0;
    }

    std::wstring ignored;
    const uintptr_t remoteKernel32 = FindModuleByName(pid, L"kernel32.dll", ignored);
    if (remoteKernel32 == 0) {
        return 0;
    }

    const uintptr_t localBase = reinterpret_cast<uintptr_t>(localKernel32);
    return remoteKernel32 + (localProc - localBase);
}

bool WaitModuleGone(DWORD pid, const std::wstring& moduleName, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        std::wstring ignored;
        if (FindModuleByName(pid, moduleName, ignored) == 0) {
            return true;
        }
        ::Sleep(50);
    }
    return false;
}

}  // namespace

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
        return false;
    }

    LUID luid{};
    bool ok = false;
    if (::LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid) != FALSE) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                                    nullptr, nullptr) != FALSE) {
            ok = ::GetLastError() == ERROR_SUCCESS;  // 权限不存在时不会报错，只会设置 ERROR_NOT_ALL_ASSIGNED
        }
    }

    ::CloseHandle(token);
    return ok;
}

bool FindProcess(const std::wstring& nameOrPid, ProcessInfo& out, std::string& error) {
    out = ProcessInfo{};

    // 纯数字按 PID 处理
    bool allDigits = !nameOrPid.empty();
    for (const wchar_t ch : nameOrPid) {
        if (ch < L'0' || ch > L'9') {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        const DWORD pid = static_cast<DWORD>(std::wcstoul(nameOrPid.c_str(), nullptr, 10));
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            error = "按 PID 打开进程失败（进程不存在或权限不足）";
            return false;
        }
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(std::size(path));
        ::QueryFullProcessImageNameW(process, 0, path, &size);
        ::CloseHandle(process);

        out.pid = pid;
        out.exePath = path;
        const size_t slash = out.exePath.find_last_of(L"\\/");
        out.exeName = (slash == std::wstring::npos) ? out.exePath : out.exePath.substr(slash + 1);
        return true;
    }

    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = "创建进程快照失败";
        return false;
    }

    std::wstring wanted = ToLower(nameOrPid);
    if (!EndsWithNoCase(wanted, L".exe")) {
        wanted += L".exe";
    }

    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (ToLower(entry.szExeFile) == wanted) {
                out.pid = entry.th32ProcessID;
                out.exeName = entry.szExeFile;
                found = true;
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);

    if (!found) {
        error = "没找到该进程（名字要含 .exe 或省略后缀）";
        return false;
    }

    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out.pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(std::size(path));
        if (::QueryFullProcessImageNameW(process, 0, path, &size) != FALSE) {
            out.exePath = path;
        }
        ::CloseHandle(process);
    }
    return true;
}

bool IsTarget64Bit(HANDLE process) {
    BOOL wow64 = FALSE;
    if (::IsWow64Process(process, &wow64) == FALSE) {
        return false;
    }
    return wow64 == FALSE;  // 不是 WOW64 就是原生 x64
}

uintptr_t FindRemoteModule(DWORD pid, const std::wstring& moduleName, std::wstring& fullPath) {
    return FindModuleByName(pid, moduleName, fullPath);
}

bool RemoteCall(HANDLE process, uintptr_t function, uintptr_t argument,
                DWORD timeoutMs, DWORD& exitCode, std::string& error) {
    exitCode = 0;

    const HANDLE thread = ::CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(function),
        reinterpret_cast<LPVOID>(argument), 0, nullptr);
    if (thread == nullptr) {
        error = "CreateRemoteThread 失败，错误码 " + std::to_string(::GetLastError());
        return false;
    }

    const DWORD wait = ::WaitForSingleObject(thread, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        error = "远程线程等待超时";
        ::CloseHandle(thread);
        return false;
    }

    ::GetExitCodeThread(thread, &exitCode);
    ::CloseHandle(thread);
    return true;
}

bool InjectDll(DWORD pid, HANDLE process, const std::wstring& dllPath, std::string& error) {
    const uintptr_t loadLibrary = RemoteKernel32Proc(pid, "LoadLibraryW");
    if (loadLibrary == 0) {
        error = "解析目标进程的 LoadLibraryW 失败";
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr) {
        error = "VirtualAllocEx 失败，错误码 " + std::to_string(::GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (::WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) == FALSE ||
        written != bytes) {
        error = "WriteProcessMemory 写入 DLL 路径失败，错误码 " + std::to_string(::GetLastError());
        ::VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    DWORD exitCode = 0;
    const bool called = RemoteCall(process, loadLibrary, reinterpret_cast<uintptr_t>(remotePath),
                                   15000, exitCode, error);
    ::VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    if (!called) {
        return false;
    }
    if (exitCode == 0) {
        error = "LoadLibraryW 返回 0（DLL 初始化失败，检查依赖与位数）";
        return false;
    }

    // 用模块名确认真的进去了：DllMain 返回 FALSE 时 HMODULE 也会是 0，
    // 但某些情况下返回值被截断，模块表才是可靠的判据。
    const size_t slash = dllPath.find_last_of(L"\\/");
    const std::wstring moduleName = (slash == std::wstring::npos) ? dllPath : dllPath.substr(slash + 1);
    std::wstring loadedFrom;
    if (FindModuleByName(pid, moduleName, loadedFrom) == 0) {
        error = "DLL 已返回但模块表里找不到，注入可能被拦截";
        return false;
    }
    return true;
}

bool EjectDll(DWORD pid, HANDLE process, const std::wstring& dllPath,
              const std::wstring& moduleName, bool callUninstall, std::string& error) {
    std::wstring loadedFrom;
    uintptr_t base = FindModuleByName(pid, moduleName, loadedFrom);
    if (base == 0) {
        return true;  // 本来就没加载，视为成功（幂等）
    }

    // 1) 先远程调 GE_Uninstall：摘钩子、停工作线程。
    //    必须在线程还活着的时候做，直接 FreeLibrary 会把钩子留成悬空指针。
    if (callUninstall) {
        uint32_t rva = 0;
        std::string exportError;
        if (FindExportRva(dllPath, "GE_Uninstall", rva, exportError)) {
            DWORD exitCode = 0;
            std::string callError;
            if (RemoteCall(process, base + rva, 0, 10000, exitCode, callError)) {
                error = "";
            } else {
                error = "调用 GE_Uninstall 失败：" + callError;
            }
        } else {
            // 已加载的可能是旧版本 / 别的同名 DLL，没有这个导出就跳过，靠 DllMain 的 detach 兜底。
            error = "已加载的 DLL 里没有 GE_Uninstall（" + exportError + "），将直接 FreeLibrary";
        }
    }

    // 2) FreeLibrary。若被注入过多次，引用计数可能 >1，循环几次直到模块消失。
    const uintptr_t freeLibrary = RemoteKernel32Proc(pid, "FreeLibrary");
    if (freeLibrary == 0) {
        error = "解析目标进程的 FreeLibrary 失败";
        return false;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD exitCode = 0;
        std::string callError;
        if (!RemoteCall(process, freeLibrary, base, 10000, exitCode, callError)) {
            error = "FreeLibrary 远程调用失败：" + callError;
            return false;
        }
        if (WaitModuleGone(pid, moduleName, 1000)) {
            return true;
        }
    }

    error = "FreeLibrary 之后模块仍然存在（可能被别的线程或工具额外持有引用）";
    return false;
}

bool CallExport(HANDLE process, uintptr_t moduleBase, const std::wstring& dllPath,
                const char* exportName, DWORD& exitCode, std::string& error) {
    exitCode = 0;

    if (moduleBase == 0) {
        error = "DLL 尚未加载";
        return false;
    }

    uint32_t rva = 0;
    std::string exportError;
    if (!FindExportRva(dllPath, exportName, rva, exportError)) {
        error = "本地 DLL 里找不到导出：" + exportError;
        return false;
    }

    return RemoteCall(process, moduleBase + rva, 0, 10000, exitCode, error);
}

}  // namespace ge
