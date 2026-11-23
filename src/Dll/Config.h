#pragma once

#include <cstdint>

#include <windows.h>

namespace ge {

// DLL 的可调参数。
//
// 启动时读 DLL 同目录下的 GameEnhancement.ini；文件不存在会用内置默认值
// 写一份出来，之后改 ini 即可，不必重新编译。
struct Config {
    uintptr_t gworldRva    = 0;      // 0 = 用 UeOffsets.h 里的内置值
    double    jumpHeight   = 180.0;  // 一次抬升的高度（UE 单位，约等于厘米）
    int       holdMs       = 300;    // 抬升后保持多久（0 = 只写一次，立刻交给物理）
    int       tickMs       = 8;      // 保持期间的重写间隔
    UINT      hotkeyVk     = 'Q';    // 触发键（虚拟键码，0x51 = Q）
    bool      zeroVelocity = true;   // 保持期间把竖直速度清零，避免落地速度累加
    uintptr_t worldZOffset = 0;      // 0 = 用 UeOffsets.h 里的内置值（ComponentToWorld 的 Z）

    // --- 动作放置（红区也能落位）---
    bool previewFix           = true;   // 总开关；0 = 完全不装鼠标钩子
    bool previewLogOnly       = false;  // 只记录状态、不写内存（用来观察游戏自己的判定）
    bool previewOnlyUnsnapped = true;   // 只在"游戏判定为无效"时介入，避免破坏正常放置
    bool previewSetSnapWall   = true;   // 改写 isSnappingWall = true
    bool previewSetFloor      = false;  // 改写 FloorPreviewZ = true（贴地分支）
    bool previewSetZ          = true;   // 把 Z 写成幽灵当前高度
    bool previewWriteHitLoc   = true;   // 把 wallHitLoaction 写成幽灵当前位置
    int  previewHoldMs        = 150;    // 点击后持续重写多久，压过下一帧的 ReceiveTick

    // --- 脚本调用追踪（方案 B 的观察阶段）---
    bool     traceScript      = true;   // 接管 manager vtable 的 ProcessEvent，只记录不改变行为
    bool     traceDryRun      = false;  // 只定位槽位并写日志，不改 vtable（首次验证用）
    uint32_t processEventSlot = 0x260;  // UE5.7 的 ProcessEvent 槽位；0 = 只用特征扫描
    bool     previewAutoCommit = true;  // 左键判定未通过时，自动补调提交函数
};

Config& Cfg();

// 读 ini（不存在则创建默认文件）。module 用来定位 ini 所在目录。
void LoadConfig(HMODULE module);

}  // namespace ge
