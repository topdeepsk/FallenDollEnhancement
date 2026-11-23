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
};

Config& Cfg();

// 读 ini（不存在则创建默认文件）。module 用来定位 ini 所在目录。
void LoadConfig(HMODULE module);

}  // namespace ge
