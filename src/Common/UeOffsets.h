#pragma once

// 与游戏（Paralogue-Win64-Shipping.exe）版本绑定的偏移表。
//
// 来源：原型期导出的 UE5 dump（DumpAll.jsonl + SDK.h，2026-11-22），
// 每条都已对照该 dump 中对应类的字段列表核验过，不是估计值。
//
// 游戏更新后若失效：重新 dump，用 Dump Explorer 查同名属性的 offset，
// 或者直接改 DLL 旁边的 GameEnhancement.ini（GWorldRVA 可从 ini 覆盖）。
//
// 注意：所有指针链偏移都是 UPROPERTY 的字节偏移，与引擎版本相关；
// GWorldRVA 是模块内相对地址（RVA），不受 ASLR 影响，但随二进制变化。

#include <cstdint>

namespace ge::ue {

// --- 全局对象 ---
// dump 记录：module_base = 0x7FF707910000，GWorld = 0x7FF7128C9F48
// 0x7FF7128C9F48 - 0x7FF707910000 = 0x0AFB9F48
inline constexpr uintptr_t kGWorldRva = 0x0AFB9F48;

// --- 解析链：UWorld -> UGameInstance -> ULocalPlayer -> APlayerController -> APawn ---
inline constexpr uint32_t kWorldOwningGameInstance   = 0x228;  // UWorld::OwningGameInstance
inline constexpr uint32_t kGameInstanceLocalPlayers  = 0x038;  // UGameInstance::LocalPlayers (TArray<ULocalPlayer*>)
inline constexpr uint32_t kLocalPlayerController     = 0x030;  // UPlayer::PlayerController
inline constexpr uint32_t kControllerPawn            = 0x2F0;  // AController::Pawn
inline constexpr uint32_t kActorRootComponent        = 0x1B8;  // AActor::RootComponent

// --- 坐标 ---
inline constexpr uint32_t kSceneRelativeLocation     = 0x148;  // USceneComponent::RelativeLocation (FVector)
inline constexpr uint32_t kFVectorStride             = 24;     // UE5 LWC：3 x float64
inline constexpr uint32_t kFVectorZOffset            = kSceneRelativeLocation + 16;

// --- 世界变换（关键）---
// USceneComponent::ComponentToWorld 是**非反射**成员，dump 里没有，必须手工定位。
// 本构建（Paralogue-Win64-Shipping.exe，2026-11-22 dump）：FTransform 起始于 +0x1F0，
// Translation 三元组在 +0x210（X +0x210 / Y +0x218 / Z +0x220）。
//
// 为什么必须写它：直接改 RelativeLocation 只是改了一个属性，**不会调用
// UpdateComponentToWorld()**，渲染与物理继续使用缓存的世界变换 —— 现象就是
// "内存里的坐标变了，但画面和角色一动不动"。必须两个一起写，值保持一致。
inline constexpr uint32_t kRootWorldLocation         = 0x210;  // USceneComponent::ComponentToWorld.Translation
inline constexpr uint32_t kRootWorldZ                = kRootWorldLocation + 16;

// USceneComponent::AttachParent —— 非零表示根组件被挂到别的组件上，
// 此时 RelativeLocation 与 ComponentToWorld 不再相等。
inline constexpr uint32_t kSceneAttachParent         = 0x0D0;

// --- 移动组件（跳跃时把竖直速度清零，避免落地速度累加） ---
inline constexpr uint32_t kCharacterMovement         = 0x338;  // ACharacter::CharacterMovement
inline constexpr uint32_t kCmcVelocity               = 0x0D8;  // UMovementComponent::Velocity (FVector)
inline constexpr uint32_t kCmcVelocityZ              = kCmcVelocity + 16;

// TArray 布局（UE5）：Data +0x00 / Num +0x08 / Max +0x0C
inline constexpr uint32_t kTArrayNum                 = 0x08;

}  // namespace ge::ue
