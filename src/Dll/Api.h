#pragma once

// 被注入 DLL 对外暴露的 C ABI。
//
// 注入器在目标进程里远程调用这几个导出：
//   GE_Uninstall  —— 重复注入前先干净卸载（停钩子、停线程）
//   GE_Install    —— 一般不直接调，DLL 自己会在加载后自动安装
//   GE_QueryState —— 0 未安装 / 1 已安装
//
// 全部是 extern "C"，名字不做修饰，方便按名字从 PE 导出表里取 RVA。

#ifdef GE_BUILD_DLL
#define GE_API extern "C" __declspec(dllexport)
#else
#define GE_API extern "C" __declspec(dllimport)
#endif

// 返回值：0 = 成功；负数 = 失败（见实现里的 GE_ERR_*）。
GE_API int GE_Install();
GE_API int GE_Uninstall();
GE_API int GE_QueryState();
GE_API unsigned GE_AbiVersion();

// 立刻触发一次跳跃，等价于按一下热键。
// 用途：不走键盘也能验证链路，注入器用 --jump 直接调它。
GE_API int GE_JumpNow();

// 动作放置（红区也能落位）：
//   GE_PreviewState  —— 0 找不到管理器 / 1 有管理器但不在预览 / 2 预览中，并写日志
//   GE_ForcePlaceNow —— 立即按当前幽灵位置改写一次判定，返回写入项数（-1 = 失败）
GE_API int GE_PreviewState();
GE_API int GE_ForcePlaceNow();

// 脚本追踪：返回 ProcessEvent 的 vtable 槽位（-1 = 未接管）。
GE_API int GE_ScriptTraceState();

// 脚本追踪：把抓到的点击调用序列写进日志。
GE_API int GE_TraceDump();

// 直接调用提交函数（不经过左键判定）。
GE_API int GE_ForceCommit();

// 实验：按 call.txt 里的“类名/函数名”在游戏线程调一次。
GE_API int GE_CallFromFile();
