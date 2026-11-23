#include "Dll/KeyHook.h"

#include "Common/Win32Util.h"
#include "Dll/Player.h"
#include "Dll/ScriptTrace.h"

namespace ge {

namespace {

HHOOK g_hook = nullptr;
HHOOK g_mouseHook = nullptr;
HANDLE g_hookThread = nullptr;
DWORD g_hookThreadId = 0;
HMODULE g_hookModule = nullptr;
UINT g_hotkey = 'Q';
volatile LONG g_stop = 0;

bool IsGameForeground() {
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(foreground, &pid);
    return pid == ::GetCurrentProcessId();
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam != 0) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        // 注入的按键（远程注入器、宏）不算，避免被别的工具误触发。
        if (isDown && (info->flags & LLKHF_INJECTED) == 0 && IsGameForeground()) {
            if (info->vkCode == g_hotkey) {
                RequestJump();
            } else if (info->vkCode == VK_SPACE) {
                // 角色模式下 H 的输入被停用，空格没人处理，这里转发给房间管理器的切镜头事件。
                if (ScriptTraceInPlayerMode()) {
                    ScriptTraceRequestCameraSwitch();
                }
            } else if (info->vkCode == 'P') {
                // 角色模式：角色可见可控 + 镜头跟随 + 动作继续；再按一次回 H 模式。
                ScriptTraceRequestRestore();
            }
        }
    }

    // 永远放行：游戏自己的按键必须照常收到。
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// 角色模式下由我们自己接管鼠标视角：H 期间鼠标事件被 A_FreeCameraPawn 独占，
// 把它停掉后就没有任何一方处理鼠标了，所以这里把位移转给 ScriptTrace 去驱动控制器。
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam != 0 && wParam == WM_MOUSEMOVE && IsGameForeground()) {
        static POINT last = {};
        static bool hasLast = false;
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (hasLast) {
            ScriptTraceAddLook(info->pt.x - last.x, info->pt.y - last.y);
        }
        last = info->pt;
        hasLast = true;
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI HookThreadProc(LPVOID) {
    g_hookThreadId = ::GetCurrentThreadId();

    // 先把消息队列建出来，否则别的线程 PostThreadMessage(WM_QUIT) 会失败。
    MSG msg;
    ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

    g_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_hookModule, 0);
    if (g_hook == nullptr) {
        LogLine("键盘钩子：SetWindowsHookEx 失败 (%lu)", ::GetLastError());
        return 0;
    }
    g_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_hookModule, 0);
    LogLine("键盘钩子：已安装，触发键 vk=0x%02X；鼠标钩子=%s", g_hotkey,
            g_mouseHook != nullptr ? "已装" : "失败");

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g_mouseHook != nullptr) {
        ::UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_hook != nullptr) {
        ::UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        LogLine("键盘钩子：已卸载");
    }
    return 0;
}

}  // namespace

bool StartKeyHook(HMODULE hSelf, UINT virtualKey) {
    if (g_hookThread != nullptr) {
        return true;
    }

    g_hookModule = hSelf;
    g_hotkey = virtualKey;
    InterlockedExchange(&g_stop, 0);

    g_hookThread = ::CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
    if (g_hookThread == nullptr) {
        LogLine("键盘钩子：CreateThread 失败 (%lu)", ::GetLastError());
        return false;
    }

    // 等钩子真正装上再返回，否则调用方以为装好了、其实还在建线程。
    for (int i = 0; i < 100; ++i) {
        if (g_hook != nullptr) {
            return true;
        }
        ::Sleep(20);
    }

    LogLine("键盘钩子：超时未安装成功");
    StopKeyHook();
    return false;
}

void StopKeyHook() {
    if (g_hookThread == nullptr) {
        return;
    }

    InterlockedExchange(&g_stop, 1);
    if (g_hookThreadId != 0) {
        ::PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);  // 让 GetMessage 返回 0
    }

    if (::WaitForSingleObject(g_hookThread, 3000) != WAIT_OBJECT_0) {
        LogLine("键盘钩子：线程 3s 内未退出");
    }

    // 兜底：线程异常退出时钩子可能还挂着，卸载 DLL 会留下悬空钩子。
    if (g_hook != nullptr) {
        ::UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }

    ::CloseHandle(g_hookThread);
    g_hookThread = nullptr;
    g_hookThreadId = 0;
}

bool IsKeyHookInstalled() {
    return g_hook != nullptr;
}

}  // namespace ge
