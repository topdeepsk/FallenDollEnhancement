#pragma once

#include <windows.h>

namespace ge {

// 安装 / 卸载低级键盘钩子（WH_KEYBOARD_LL）。
//
// 钩子必须装在**带消息循环的线程**里：低级钩子的回调是由安装线程的消息循环派发的，
// 所以这里单独开一个线程跑 GetMessage 循环，而不是挂在任意线程上。
// hSelf 用于 SetWindowsHookEx 的模块句柄（卸载 DLL 前必须能正确解挂）。
bool StartKeyHook(HMODULE hSelf, UINT virtualKey);
void StopKeyHook();
bool IsKeyHookInstalled();

}  // namespace ge
