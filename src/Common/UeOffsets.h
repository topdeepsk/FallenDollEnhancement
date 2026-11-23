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

// UCharacterMovementComponent::MovementMode（TEnumAsByte<EMovementMode>，1 字节）
// 布局来自运行中进程的属性链：MovementMode +0x241、GroundMovementMode +0x55A。
// 直接改坐标时必须把它切成 Falling，否则组件仍按 Walking 做贴地推算，会把角色拽回地面。
inline constexpr uint32_t kCmcMovementMode             = 0x241;
inline constexpr uint8_t  kMoveModeWalking             = 1;
inline constexpr uint8_t  kMoveModeFalling             = 3;

// TArray 布局（UE5）：Data +0x00 / Num +0x08 / Max +0x0C
inline constexpr uint32_t kTArrayNum                 = 0x08;

// ---------------------------------------------------------------------------
// 运行时反射（不依赖 dump 的类表）
//
// 玩法逻辑跑在 AngelScript 层：A_* / U_* 这些类在 dump 里查不到（UClass 是 ASClass），
// 函数也没有原生 Func 指针。所以功能只能"按名字在运行时找对象、找属性"。
//
// 下面是对象表本身的偏移，来自 dump 的 meta 行（gobjects / gnames 减 module_base），
// 并在运行中的进程上逐条验证过；它们是引擎结构，不随游戏内容变化，只有引擎升级才需要改。
// ---------------------------------------------------------------------------
inline constexpr uintptr_t kGObjectsRva              = 0x0AB0D3D0;  // FUObjectItem** 块指针表
inline constexpr uintptr_t kGNamesRva                = 0x0AD5C080;  // FNamePool

// UObject
inline constexpr uint32_t kUObjectClass              = 0x10;  // UClass*
inline constexpr uint32_t kUObjectName               = 0x18;  // FName{Index, Number}
inline constexpr uint32_t kUObjectOuter              = 0x20;  // UObject*

// UStruct / UClass
inline constexpr uint32_t kUStructSuper              = 0x40;  // UStruct*
inline constexpr uint32_t kUStructChildren           = 0x48;  // UField*（UFunction 链）
inline constexpr uint32_t kUStructChildProperties    = 0x50;  // FField*（属性链）
inline constexpr uint32_t kUStructPropertiesSize     = 0x58;  // uint32

// FField / FProperty（UE5.4+）
inline constexpr uint32_t kFFieldNext                = 0x18;
inline constexpr uint32_t kFFieldName                = 0x20;  // FName
inline constexpr uint32_t kFPropertyElementSize      = 0x34;  // int32
inline constexpr uint32_t kFPropertyOffset           = 0x48;  // int32，属性在实例内的字节偏移

// FNamePool
inline constexpr uint32_t kNamePoolCurrentBlock      = 0x08;
inline constexpr uint32_t kNamePoolBlocks            = 0x10;
inline constexpr uint32_t kNameEntryLenShift         = 6;      // 头 2 字节里的长度位
inline constexpr uint32_t kNameEntryWideBit          = 15;
inline constexpr uint32_t kNameEntryMaxLen           = 511;
inline constexpr uint32_t kNameBlockSize             = 0x20000;  // 每块 128KB

// GUObjectArray 的块
inline constexpr uint32_t kObjectItemStride          = 24;     // FUObjectItem
inline constexpr uint32_t kObjectItemObject          = 8;      // 对象指针在 item 内的偏移
inline constexpr uint32_t kObjectChunkSize           = 65536;  // 每块 item 数

}  // namespace ge::ue
