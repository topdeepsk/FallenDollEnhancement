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
