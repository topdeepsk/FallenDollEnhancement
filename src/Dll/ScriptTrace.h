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

// 带重试的安装：游戏刚启动时 A_ClientHPreviewManager 还不存在（先注入后加载的场景），
// 单次安装必然失败，之后也不会再试 —— 表现为"跳跃正常但放置失效"。这里每 3 秒重试。
void StartScriptTraceRetry();
void StopScriptTraceRetry();

void StopScriptTrace();
bool IsScriptTraceInstalled();

// 打印当前解析结果（vtable / 槽位 / 原函数地址），返回槽位，失败 -1。
int ScriptTraceQuery();

// 把抓到的两次点击调用序列写进日志（缓冲 A / B）。
void ScriptTraceDumpCaptures();

// 直接调用 A_ClientHManager 的两个提交函数（等价于游戏判定通过后的动作）。
// 0 = 已调用；负数 = 解析失败或未接管。
int ScriptTraceForceCommit();

}  // namespace ge
