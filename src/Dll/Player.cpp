#include "Dll/Player.h"

#include <windows.h>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"
#include "Dll/Config.h"
#include "Dll/PeerJump.h"

namespace ge {

namespace {

HANDLE g_jumpEvent = nullptr;
HANDLE g_jumpThread = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_jumpCount = 0;

// 从 GWorld 一路走到玩家的 RootComponent。
//
// 每次都重新解析，不缓存任何指针：场上同时存在多个 AvatarPawn，
// 切场景 / 换角色 / 重生都会让上一次的指针失效，缓存指针是这类工具最常见的崩溃来源。
bool ResolvePlayer(uintptr_t& outPawn, uintptr_t& outRoot) {
    outPawn = 0;
    outRoot = 0;

    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        return false;
    }

    uintptr_t world = 0;
    if (!Read(moduleBase + Cfg().gworldRva, world) || !IsUserPointer(world)) {
        return false;
    }

    uintptr_t gameInstance = 0;
    if (!Read(world + ue::kWorldOwningGameInstance, gameInstance) || !IsUserPointer(gameInstance)) {
        return false;
    }

    // LocalPlayers 是 TArray<ULocalPlayer*>：Data 在 +0x00，Num 在 +0x08。
    uintptr_t localPlayersData = 0;
    if (!Read(gameInstance + ue::kGameInstanceLocalPlayers, localPlayersData) ||
        !IsUserPointer(localPlayersData)) {
        return false;
    }

    int32_t localPlayerCount = 0;
    if (!Read(gameInstance + ue::kGameInstanceLocalPlayers + ue::kTArrayNum, localPlayerCount) ||
        localPlayerCount <= 0 || localPlayerCount > 8) {
        return false;
    }

    uintptr_t localPlayer = 0;
    if (!Read(localPlayersData, localPlayer) || !IsUserPointer(localPlayer)) {
        return false;
    }

    uintptr_t playerController = 0;
    if (!Read(localPlayer + ue::kLocalPlayerController, playerController) ||
        !IsUserPointer(playerController)) {
        return false;
    }

    uintptr_t pawn = 0;
    if (!Read(playerController + ue::kControllerPawn, pawn) || !IsUserPointer(pawn)) {
        return false;
    }

    uintptr_t root = 0;
    if (!Read(pawn + ue::kActorRootComponent, root) || !IsUserPointer(root)) {
        return false;
    }

    outPawn = pawn;
    outRoot = root;
    return true;
}

uintptr_t MovementOf(uintptr_t pawn) {
    uintptr_t movement = 0;
    if (!Read(pawn + ue::kCharacterMovement, movement) || !IsUserPointer(movement)) {
        return 0;
    }
    return movement;
}

void ZeroVerticalVelocity(uintptr_t movement) {
    const double zero = 0.0;
    Write(movement + ue::kCmcVelocityZ, zero);
}

// 切成 Falling：这是"传送式抬升"能站住的关键。
// 不改的话组件仍处于 Walking，每次 PerformMovement 都会按贴地逻辑把角色写回地面，
// 表现就是"瞬间落地"。
void ForceFalling(uintptr_t movement) {
    const uint8_t falling = ue::kMoveModeFalling;
    Write(movement + ue::kCmcMovementMode, falling);
}

// 一次跳跃 = 读出当前世界 Z，一次性抬到目标高度，保持 holdMs 再放手。
//
// "瞬间提高"就体现在这里：不做插值、不做缓动，第一帧就已经在目标高度上，
// 后续的重写只是防止 CharacterMovement 当成悬空把人立刻拽回地面。
//
// 关键点：必须**同时**写 RelativeLocation 和 ComponentToWorld.Translation。
// 只写前者不会调用 UpdateComponentToWorld()，渲染与物理会继续用缓存的世界变换，
// 现象就是"内存里坐标变了但画面完全不动"。
void DoJump() {
    uintptr_t pawn = 0;
    uintptr_t root = 0;
    if (!ResolvePlayer(pawn, root)) {
        LogLine("跳跃：解析玩家失败（可能还在加载界面 / 无 Pawn）");
        return;
    }

    // 世界坐标是权威值；RelativeLocation 可能滞后（甚至被历史写入污染过）。
    double worldZ = 0.0;
    if (!Read(root + Cfg().worldZOffset, worldZ)) {
        LogLine("跳跃：读不到世界坐标 Z（WorldZOffset=0x%llX 可能已失效）",
                static_cast<unsigned long long>(Cfg().worldZOffset));
        return;
    }
    double relZ = 0.0;
    Read(root + ue::kFVectorZOffset, relZ);

    uintptr_t attachParent = 0;
    Read(root + ue::kSceneAttachParent, attachParent);
    const bool attached = attachParent != 0;

    // 位置由服务端权威：本地进程里 PC/Pawn 的 Role 不是 Authority 时，
    // 写本地坐标会在下一个网络更新（~32ms）被覆盖回去，必须写到服务端进程。
    uint8_t role = 0;
    const bool localAuthority = Read(pawn + 0x168, role) && role == 3;
    if (!localAuthority) {
        double world[3] = {};
        if (!ReadMemory(root + Cfg().worldZOffset - 16, world, sizeof(world))) {
            LogLine("跳跃：读不到世界坐标，放弃");
            return;
        }
        if (PeerJumpUp(world, Cfg().jumpHeight)) {
            LogLine("跳跃 #%ld：已交给服务端执行（本地 Role=%u，非权威）",
                    InterlockedIncrement(&g_jumpCount), static_cast<unsigned>(role));
            return;
        }
        LogLine("跳跃：服务端不可用，退回本地写入（会被网络同步覆盖）");
    }

    const double targetWorld = worldZ + Cfg().jumpHeight;
    // 根组件无父级时相对坐标等于世界坐标；被 attach 时保持原有差值。
    const double targetRel = attached ? (relZ + Cfg().jumpHeight) : targetWorld;

    const int holdMs = Cfg().holdMs;
    const int tickMs = (Cfg().tickMs > 0) ? Cfg().tickMs : 8;

    LogLine("跳跃 #%ld：pawn=0x%llX worldZ %.1f -> %.1f（relZ %.1f%s，保持 %dms）",
            InterlockedIncrement(&g_jumpCount), static_cast<unsigned long long>(pawn),
            worldZ, targetWorld, relZ, attached ? "，已 attach" : "", holdMs);

    for (int elapsed = 0;; elapsed += tickMs) {
        // 保持期间重新解析一次：期间可能切场景/换角色，旧指针不能继续写。
        if (!ResolvePlayer(pawn, root)) {
            LogLine("跳跃：保持期间玩家失效，提前结束");
            return;
        }

        Write(root + Cfg().worldZOffset, targetWorld);
        Write(root + ue::kFVectorZOffset, targetRel);
        const uintptr_t movement = MovementOf(pawn);
        if (movement != 0) {
            ForceFalling(movement);
            if (Cfg().zeroVelocity) {
                ZeroVerticalVelocity(movement);
            }
        }

        if (elapsed >= holdMs) {
            break;
        }
        ::Sleep(static_cast<DWORD>(tickMs));
    }
}

DWORD WINAPI JumpThreadProc(LPVOID) {
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        const DWORD wait = ::WaitForSingleObject(g_jumpEvent, 100);
        // 先确认不是"卸载时的唤醒"：StopJumpWorker 会 SetEvent 催线程退出，
        // 若不检查标志，退出前会白跳一次。
        if (wait == WAIT_OBJECT_0 && InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
            DoJump();
        }
    }
    return 0;
}

}  // namespace

bool StartJumpWorker() {
    if (g_jumpThread != nullptr) {
        return true;
    }

    g_jumpEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 自动重置
    if (g_jumpEvent == nullptr) {
        LogLine("跳跃：CreateEvent 失败 (%lu)", ::GetLastError());
        return false;
    }

    InterlockedExchange(&g_stop, 0);
    g_jumpThread = ::CreateThread(nullptr, 0, JumpThreadProc, nullptr, 0, nullptr);
    if (g_jumpThread == nullptr) {
        LogLine("跳跃：CreateThread 失败 (%lu)", ::GetLastError());
        ::CloseHandle(g_jumpEvent);
        g_jumpEvent = nullptr;
        return false;
    }
    return true;
}

void StopJumpWorker() {
    if (g_jumpThread == nullptr) {
        return;
    }

    InterlockedExchange(&g_stop, 1);
    if (g_jumpEvent != nullptr) {
        ::SetEvent(g_jumpEvent);  // 立刻唤醒，让它看到 stop 标志
    }

    if (::WaitForSingleObject(g_jumpThread, 3000) != WAIT_OBJECT_0) {
        LogLine("跳跃：工作线程 3s 内未退出，可能卡在内存读写");
    }

    ::CloseHandle(g_jumpThread);
    g_jumpThread = nullptr;

    if (g_jumpEvent != nullptr) {
        ::CloseHandle(g_jumpEvent);
        g_jumpEvent = nullptr;
    }
}

void RequestJump() {
    if (g_jumpEvent != nullptr) {
        ::SetEvent(g_jumpEvent);
    }
}

}  // namespace ge
