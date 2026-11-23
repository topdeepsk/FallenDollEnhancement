#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

namespace ge {

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring exeName;
    std::wstring exePath;
};

bool EnableDebugPrivilege();

// nameOrPid 支持 "Paralogue-Win64-Shipping.exe" / "Paralogue-Win64-Shipping" / "12345"
bool FindProcess(const std::wstring& nameOrPid, ProcessInfo& out, std::string& error);

bool IsTarget64Bit(HANDLE process);

// 目标进程里某个模块的基址；没有返回 0。
uintptr_t FindRemoteModule(DWORD pid, const std::wstring& moduleName, std::wstring& fullPath);

// 在目标进程里用 CreateRemoteThread 调一个单参数函数，等待结束并取返回值。
bool RemoteCall(HANDLE process, uintptr_t function, uintptr_t argument,
                DWORD timeoutMs, DWORD& exitCode, std::string& error);

// 注入：VirtualAllocEx + WriteProcessMemory(路径) + CreateRemoteThread(LoadLibraryW)
bool InjectDll(DWORD pid, HANDLE process, const std::wstring& dllPath, std::string& error);

// 卸载：先在目标里远程调 GE_Uninstall（摘钩子、停线程），再 FreeLibrary。
// 重复注入前必须先走这一步，否则旧实例的钩子会留在系统里。
bool EjectDll(DWORD pid, HANDLE process, const std::wstring& dllPath,
              const std::wstring& moduleName, bool callUninstall, std::string& error);

// 远程调用已加载 DLL 里的一个导出（无参、返回 int）。
// 用于 --jump：从注入器直接触发一次跳跃，不必依赖键盘。
bool CallExport(HANDLE process, uintptr_t moduleBase, const std::wstring& dllPath,
                const char* exportName, DWORD& exitCode, std::string& error);

}  // namespace ge
