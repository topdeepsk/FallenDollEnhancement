#include "Dll/Api.h"

#include <windows.h>

#include "Common/Win32Util.h"
#include "Dll/Config.h"
#include "Dll/KeyHook.h"
#include "Dll/Player.h"

using namespace ge;

namespace {

HMODULE g_self = nullptr;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
volatile LONG g_installed = 0;

// GE_Install / GE_Uninstall 的返回码
enum InstallResult : int {
    kGeOk = 0,
    kGeErrJumpWorker = -2,
    kGeErrKeyHook = -3,
};

DWORD WINAPI AutoInstallProc(LPVOID) {
    // 不在 DllMain 里做安装：DllMain 处于 loader lock 下，
    // 创建线程、SetWindowsHookEx、读文件都可能死锁。
    GE_Install();
    return 0;
}

}  // namespace

GE_API int GE_QueryState() {
    return g_installed != 0 ? 1 : 0;
}

GE_API unsigned GE_AbiVersion() {
    return 1u;
}

GE_API int GE_JumpNow() {
    if (g_installed == 0) {
        return -1;
    }
    RequestJump();
    return kGeOk;
}

GE_API int GE_Install() {
    if (!g_lockReady) {
        return kGeErrJumpWorker;
    }

    ::EnterCriticalSection(&g_lock);

    if (g_installed != 0) {
        ::LeaveCriticalSection(&g_lock);
        return kGeOk;  // 幂等：重复安装直接返回成功
    }

    LoadConfig(g_self);

    if (!StartJumpWorker()) {
        ::LeaveCriticalSection(&g_lock);
        return kGeErrJumpWorker;
    }

    if (!StartKeyHook(g_self, Cfg().hotkeyVk)) {
        StopJumpWorker();
        ::LeaveCriticalSection(&g_lock);
        return kGeErrKeyHook;
    }

    ::InterlockedExchange(&g_installed, 1);
    ::LeaveCriticalSection(&g_lock);

    LogLine("安装完成：按 0x%02X 触发跳跃（高度 %.0f，保持 %dms）",
            Cfg().hotkeyVk, Cfg().jumpHeight, Cfg().holdMs);
    return kGeOk;
}

GE_API int GE_Uninstall() {
    if (!g_lockReady) {
        return kGeOk;
    }

    ::EnterCriticalSection(&g_lock);

    if (g_installed == 0) {
        ::LeaveCriticalSection(&g_lock);
        return kGeOk;  // 幂等：未安装也算成功
    }

    // 先落标志，避免卸载途中又有按键请求排进队列。
    ::InterlockedExchange(&g_installed, 0);

    StopKeyHook();    // 必须是先摘钩子：钩子回调会调用 RequestJump
    StopJumpWorker();

    ::LeaveCriticalSection(&g_lock);

    LogLine("已卸载");
    return kGeOk;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        g_self = module;
        ::DisableThreadLibraryCalls(module);

        // 日志与 GameEnhancement.ini 放在同一目录（DLL 所在目录），
        // 必须在任何 LogLine 之前设置好。
        SetLogDirectory(GetDirectoryOf(GetModuleFilePath(module)));

        ::InitializeCriticalSection(&g_lock);
        g_lockReady = true;

        const HANDLE thread = ::CreateThread(nullptr, 0, AutoInstallProc, nullptr, 0, nullptr);
        if (thread != nullptr) {
            ::CloseHandle(thread);  // 线程继续跑，句柄不用了
        } else {
            LogLine("DLL 加载：创建安装线程失败 (%lu)", ::GetLastError());
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        // reserved != nullptr 表示进程正在退出：此时不能再等线程、也不能写日志，
        // 交给系统回收即可（钩子随进程结束一起失效）。
        if (reserved == nullptr && g_lockReady) {
            GE_Uninstall();
            ::DeleteCriticalSection(&g_lock);
            g_lockReady = false;
        }
        break;
    default:
        break;
    }
    return TRUE;
}
