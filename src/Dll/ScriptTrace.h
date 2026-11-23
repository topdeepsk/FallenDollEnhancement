#pragma once

namespace ge {

// 脚本调用追踪：只读观察，不改游戏逻辑。
//
// 这个游戏的玩法函数是 AngelScript 的 UFunction，没有原生 Func 指针，
// 调用统一走 UObject::ProcessEvent（虚函数，vtable 槽位 0x260 @ UE5.7）。
// 这里把 A_ClientHPreviewManager 的 vtable 里那一格换成自己的 thunk，
// 记录"谁调了什么函数"，然后原样转给原函数。
//
// 目的：对比绿色（能放）和红色（不能放）两次点击，看游戏自己到底调了哪些函数，
// 从而确定绕过点。thunk 里只做读+写日志，任何一步失败都不影响调用本身。
bool StartScriptTrace();
void StopScriptTrace();
bool IsScriptTraceInstalled();

// 打印当前解析结果（vtable / 槽位 / 原函数地址），返回槽位，失败 -1。
int ScriptTraceQuery();

// 把抓到的两次点击调用序列写进日志（缓冲 A / B）。
void ScriptTraceDumpCaptures();

// 直接调用 A_ClientHManager 的两个提交函数（等价于游戏判定通过后的动作）。
// 0 = 已调用；负数 = 解析失败或未接管。
int ScriptTraceForceCommit();

// 从 DLL 同目录的 call.txt 读"类名/函数名"，让游戏线程调一次（实验用）。
int ScriptTraceCallFromFile();

// P 键：恢复玩家角色的显示与控制，动作继续播放。
// 可从任意线程调用；实际调用发生在游戏线程的 ProcessEvent thunk 里。
void ScriptTraceRequestRestore();

// O 键（实验）：走游戏退出流程，同时拦下"停止播放/销毁"的反射调用。
// 用来验证销毁是否走反射——若走，就能同时拿到原生镜头/UI 和持续播放。
void ScriptTraceRequestQuitExperiment();

// 鼠标位移（由 WH_MOUSE_LL 钩子调用）。只在"玩家角色模式"下生效。
void ScriptTraceAddLook(long dx, long dy);

// 是否处于"玩家角色模式"（P 键切换后的状态）。
bool ScriptTraceInPlayerMode();

// 把空格转发给普通模式下的切镜头处理函数。
void ScriptTraceRequestCameraSwitch();

}  // namespace ge
