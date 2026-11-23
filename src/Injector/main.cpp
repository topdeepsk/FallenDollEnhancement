#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

#include "Common/Win32Util.h"
#include "Injector/Injector.h"

namespace {

std::string ToUtf8(const std::wstring& text) {
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

std::wstring FromUtf8(const char* text) {
    const int wsize = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wsize <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(wsize - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), wsize);
    return out;
}

std::wstring DirectoryOf(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : filePath.substr(0, slash + 1);
}

void PrintUsage() {
    std::printf(
        "GameEnhancement 注入器\n"
        "\n"
        "用法：\n"
        "  ge_inject.exe <进程名|PID> [选项]\n"
        "\n"
        "选项：\n"
        "  --dll <路径>         指定要注入的 DLL（默认：exe 同目录的 GameEnhancement.dll）\n"
        "  --eject              只卸载已注入的实例，不重新注入\n"
        "  --jump               让已注入的实例立刻跳一次（用于验证，不必按 Q）\n"
        "  --preview            打印动作放置的预览状态（不点鼠标，只读）\n"
        "  --place              按当前幽灵位置强制改写一次放置判定（排错用）\n"
        "  --script             打印 ProcessEvent 接管状态（vtable 槽位，只读）\n"
        "  --dump               把抓到的左键点击调用序列写进日志\n"
        "  --commit             直接调用提交函数（不经过左键判定）\n"
        "  --no-unload          不先卸载旧实例，直接注入（默认会先卸再注）\n"
        "  --skip-uninstall     卸载时跳过 GE_Uninstall，只做 FreeLibrary\n"
        "  --wait <秒>          等待进程出现，最多等待指定秒数\n"
        "  -h, --help           显示本帮助\n"
        "\n"
        "示例：\n"
        "  ge_inject.exe Paralogue-Win64-Shipping.exe\n"
        "  ge_inject.exe Paralogue-Win64-Shipping.exe --wait 60\n"
        "  ge_inject.exe 12345 --dll D:\\tools\\GameEnhancement.dll\n"
        "  ge_inject.exe Paralogue-Win64-Shipping.exe --eject\n");
}

struct Options {
    std::wstring target;
    std::wstring dllPath;
    bool ejectOnly = false;
    bool jumpOnly = false;
    bool previewOnly = false;
    bool placeOnly = false;
    bool scriptState = false;
    bool dumpTrace = false;
    bool commitNow = false;
    bool skipUnload = false;
    bool skipUninstallCall = false;
    int waitSeconds = 0;
};

bool ParseArgs(int argc, char** argv, Options& options) {
    const std::wstring exePath = ge::GetModuleFilePath(nullptr);
    options.dllPath = DirectoryOf(exePath) + L"GameEnhancement.dll";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            return false;
        }
        if (std::strcmp(arg, "--eject") == 0) {
            options.ejectOnly = true;
        } else if (std::strcmp(arg, "--jump") == 0) {
            options.jumpOnly = true;
            options.skipUnload = true;  // 触发跳跃不该先卸载再装
        } else if (std::strcmp(arg, "--preview") == 0) {
            options.previewOnly = true;
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--place") == 0) {
            options.placeOnly = true;
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--script") == 0) {
            options.scriptState = true;
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--dump") == 0) {
            options.dumpTrace = true;
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--commit") == 0) {
            options.commitNow = true;
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--no-unload") == 0) {
            options.skipUnload = true;
        } else if (std::strcmp(arg, "--skip-uninstall") == 0) {
            options.skipUninstallCall = true;
        } else if (std::strcmp(arg, "--dll") == 0 || std::strcmp(arg, "--wait") == 0) {
            const bool isDll = std::strcmp(arg, "--dll") == 0;
            if (i + 1 >= argc) {
                std::printf("错误：%s 后面缺少参数\n\n", arg);
                return false;
            }
            const char* value = argv[++i];
            if (isDll) {
                options.dllPath = FromUtf8(value);
            } else {
                options.waitSeconds = std::atoi(value);
            }
        } else if (arg[0] == '-') {
            std::printf("错误：未知选项 %s\n\n", arg);
            return false;
        } else if (options.target.empty()) {
            options.target = FromUtf8(arg);
        } else {
            std::printf("错误：多余的参数 %s\n\n", arg);
            return false;
        }
    }

    if (options.target.empty()) {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    Options options;
    if (!ParseArgs(argc, argv, options)) {
        PrintUsage();
        return 1;
    }

    ge::EnableDebugPrivilege();  // 失败不致命：对同权限的游戏不需要它

    std::printf("目标进程 : %s\n", ToUtf8(options.target).c_str());
    std::printf("DLL      : %s\n", ToUtf8(options.dllPath).c_str());
    std::printf("模式     : %s\n\n", options.ejectOnly ? "仅卸载" : "注入（重复注入会先卸载）");

    // --- 1. 等进程出现 ---
    ge::ProcessInfo info;
    std::string error;
    const int deadline = options.waitSeconds > 0 ? ::GetTickCount() + options.waitSeconds * 1000 : 0;
    for (;;) {
        if (ge::FindProcess(options.target, info, error)) {
            break;
        }
        if (deadline == 0 || static_cast<int>(::GetTickCount()) > deadline) {
            std::printf("[失败] %s\n", error.c_str());
            return 1;
        }
        ::Sleep(500);
    }
    std::printf("[1/4] 找到进程 PID=%lu  %s\n", static_cast<unsigned long>(info.pid),
                ToUtf8(info.exeName).c_str());

    // --- 2. 打开目标 ---
    const HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                             PROCESS_VM_READ | SYNCHRONIZE,
                                         FALSE, info.pid);
    if (process == nullptr) {
        std::printf("[失败] OpenProcess 失败（错误码 %lu）。若游戏以管理员启动，本程序也需要管理员权限。\n",
                    static_cast<unsigned long>(::GetLastError()));
        return 1;
    }

    if (!ge::IsTarget64Bit(process)) {
        std::printf("[失败] 目标是 32 位进程，本工具只提供 x64 DLL。\n");
        ::CloseHandle(process);
        return 1;
    }

    const size_t slash = options.dllPath.find_last_of(L"\\/");
    const std::wstring moduleName =
        (slash == std::wstring::npos) ? options.dllPath : options.dllPath.substr(slash + 1);
    std::printf("[2/4] 目标为 x64，DLL 模块名 %s\n", ToUtf8(moduleName).c_str());

    // --- 3. 卸载旧实例（重复注入的关键一步） ---
    std::wstring loadedFrom;
    const uintptr_t existing = ge::FindRemoteModule(info.pid, moduleName, loadedFrom);
    if (existing != 0) {
        std::printf("[3/4] 检测到已加载 0x%llX  %s\n",
                    static_cast<unsigned long long>(existing), ToUtf8(loadedFrom).c_str());

        if (options.skipUnload) {
            std::printf("      已指定 --no-unload，跳过卸载\n");
        } else {
            const bool ok = ge::EjectDll(info.pid, process, options.dllPath, moduleName,
                                         !options.skipUninstallCall, error);
            if (!ok) {
                std::printf("[失败] 卸载旧实例失败：%s\n", error.c_str());
                ::CloseHandle(process);
                return 1;
            }
            std::printf("      已卸载旧实例\n");
        }
    } else {
        std::printf("[3/4] 当前未加载该 DLL，无需卸载\n");
    }

    if (options.ejectOnly) {
        ::CloseHandle(process);
        std::printf("\n完成：仅卸载模式。\n");
        return 0;
    }

    // --- 可选：直接调一个导出做验证（跳跃 / 放置状态 / 强制落位），不必切回游戏操作 ---
    if (options.jumpOnly || options.previewOnly || options.placeOnly || options.scriptState ||
        options.dumpTrace || options.commitNow) {
        uintptr_t base = existing;
        if (base == 0) {
            if (!ge::InjectDll(info.pid, process, options.dllPath, error)) {
                std::printf("[失败] 注入失败：%s\n", error.c_str());
                ::CloseHandle(process);
                return 1;
            }
            std::wstring ignored;
            base = ge::FindRemoteModule(info.pid, moduleName, ignored);
            // 等 DllMain 的安装线程把钩子、worker、首次反射扫描跑完。
            ::Sleep(options.jumpOnly ? 200 : 1500);
        }

        const char* exportName = options.jumpOnly ? "GE_JumpNow"
                                 : options.placeOnly ? "GE_ForcePlaceNow"
                                 : options.scriptState ? "GE_ScriptTraceState"
                                 : options.dumpTrace ? "GE_TraceDump"
                                 : options.commitNow ? "GE_ForceCommit"
                                                     : "GE_PreviewState";
        DWORD code = 0;
        if (!ge::CallExport(process, base, options.dllPath, exportName, code, error)) {
            std::printf("[失败] 调用 %s 失败：%s\n", exportName, error.c_str());
            ::CloseHandle(process);
            return 1;
        }

        if (options.jumpOnly) {
            std::printf("[4/4] 已触发跳跃，返回 %lu（0 = 成功）\n", static_cast<unsigned long>(code));
        } else if (options.previewOnly) {
            static const char* kState[] = {"找不到预览管理器", "管理器在，但当前不在预览", "预览中"};
            const char* text = (code <= 2) ? kState[code] : "未知";
            std::printf("[4/4] 预览状态：%s（返回值 %lu）\n", text, static_cast<unsigned long>(code));
        } else if (options.scriptState) {
            std::printf("[4/4] ProcessEvent 槽位：0x%lX（-1 = 未接管）\n",
                        static_cast<unsigned long>(code));
        } else if (options.dumpTrace) {
            std::printf("[4/4] 已导出捕获序列，返回 %lu\n", static_cast<unsigned long>(code));
        } else if (options.commitNow) {
            std::printf("[4/4] 强制提交返回 %lu（0 = 已调用，负数 = 解析失败）\n",
                        static_cast<unsigned long>(code));
        } else {
            std::printf("[4/4] 强制落位返回 %lu（3 = 已改写，2 = 本来有效，1 = 不在预览，0 = 未就绪）\n",
                        static_cast<unsigned long>(code));
        }
        std::printf("      详情见 %s\n",
                    ToUtf8(ge::GetDirectoryOf(options.dllPath) + L"GameEnhancement.log").c_str());
        ::CloseHandle(process);
        return (code != 0 && code != static_cast<DWORD>(-1)) ? 0 : 1;
    }

    // --- 4. 注入 ---
    if (::GetFileAttributesW(options.dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("[失败] DLL 文件不存在：%s\n", ToUtf8(options.dllPath).c_str());
        ::CloseHandle(process);
        return 1;
    }

    if (!ge::InjectDll(info.pid, process, options.dllPath, error)) {
        std::printf("[失败] 注入失败：%s\n", error.c_str());
        ::CloseHandle(process);
        return 1;
    }
    std::printf("[4/4] 注入成功\n");

    ::CloseHandle(process);
    return 0;
}
