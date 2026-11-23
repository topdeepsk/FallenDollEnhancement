#include "Dll/KeyHook.h"

#include "Common/Win32Util.h"
#include "Dll/Player.h"

namespace ge {

namespace {

HHOOK g_hook = nullptr;
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
        if (info->vkCode == g_hotkey) {
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (isDown) {
                // 注入的按键（远程注入器、宏）不算，避免被别的工具误触发。
                if ((info->flags & LLKHF_INJECTED) == 0 && IsGameForeground()) {
                    RequestJump();
                }
            }
        }
    }

    // 永远放行：游戏自己的 Q 输入必须照常收到。
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
    LogLine("键盘钩子：已安装，触发键 vk=0x%02X", g_hotkey);

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
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
