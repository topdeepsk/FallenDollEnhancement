#include "Dll/PreviewBypass.h"

#include <cmath>
#include <cstring>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"
#include "Dll/Config.h"
#include "Dll/UeRuntime.h"

namespace ge {

namespace {

// 运行时解析出来的定位信息。只有后台线程会写，鼠标回调只读（加锁拷一份）。
struct Fixup {
    bool      ready        = false;  // manager + 四个关键属性都定位成功
    uintptr_t managerClass = 0;
    uintptr_t manager      = 0;
    uintptr_t groupClass   = 0;
    uint32_t  managerSize  = 0;

    ue::PropInfo preview;     // ObjectProperty -> A_ClientHPreviewGroup
    ue::PropInfo snapItem;    // BoolProperty  isSnappingItem
    ue::PropInfo snapWall;    // BoolProperty  isSnappingWall
    ue::PropInfo floorZ;      // BoolProperty  FloorPreviewZ
    ue::PropInfo zField;      // DoubleProperty Z
    ue::PropInfo hitLoc;      // StructProperty wallHitLoaction (FVector)
    ue::PropInfo groupTarget; // ObjectProperty Target（SceneComponent）
};

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
Fixup g_fixup;

HHOOK  g_mouseHook = nullptr;
HANDLE g_hookThread = nullptr;
DWORD  g_hookThreadId = 0;
HMODULE g_selfModule = nullptr;
volatile LONG g_stop = 0;

HANDLE g_worker = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_holdUntil = 0;   // GetTickCount 截止时间
volatile LONG g_lastRefresh = 0;
volatile LONG g_appliedCount = 0;

bool IsGameForeground() {
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(foreground, &pid);
    return pid == ::GetCurrentProcessId();
}

Fixup CopyFixup() {
    Fixup copy;
    if (!g_lockReady) {
        return copy;
    }
    ::EnterCriticalSection(&g_lock);
    copy = g_fixup;
    ::LeaveCriticalSection(&g_lock);
    return copy;
}

void StoreFixup(const Fixup& fixup) {
    if (!g_lockReady) {
        return;
    }
    ::EnterCriticalSection(&g_lock);
    g_fixup = fixup;
    ::LeaveCriticalSection(&g_lock);
}

uintptr_t ReadPtr(uintptr_t address) {
    uintptr_t value = 0;
    Read(address, value);
    return value;
}

bool ReadBool(uintptr_t object, const ue::PropInfo& prop, bool& out) {
    if (!prop.valid) {
        out = false;
        return false;
    }
    uint8_t byte = 0;
    if (!Read(object + prop.offset, byte)) {
        return false;
    }
    out = byte != 0;
    return true;
}

bool ReadVector(uintptr_t address, double (&out)[3]) {
    uint8_t buffer[24] = {};
    if (!ReadMemory(address, buffer, sizeof(buffer))) {
        return false;
    }
    memcpy(out, buffer, sizeof(out));
    return true;
}

// 直接写：目标页本来就是可写堆页，不需要 VirtualProtect。
// 调用前都做过"同地址读成功 + 偏移越界检查"，读得到就写得进。
bool WriteBool(uintptr_t object, const ue::PropInfo& prop, bool value) {
    if (!prop.valid) {
        return false;
    }
    const uint8_t byte = value ? 1 : 0;
    return WriteMemory(object + prop.offset, &byte, sizeof(byte));
}

bool WriteDouble(uintptr_t object, const ue::PropInfo& prop, double value) {
    if (!prop.valid) {
        return false;
    }
    return WriteMemory(object + prop.offset, &value, sizeof(value));
}

bool WriteVector(uintptr_t object, const ue::PropInfo& prop, const double (&value)[3]) {
    if (!prop.valid) {
        return false;
    }
    return WriteMemory(object + prop.offset, value, sizeof(value));
}

// 解析 A_ClientHPreviewManager 的类 / 实例 / 属性偏移。耗时几百毫秒，只在后台线程调。
bool BuildFixup(Fixup& fixup) {
    if (!ue::InitRuntime()) {
        return false;
    }

    uintptr_t managerClass = 0;
    if (!ue::FindClassByName("A_ClientHPreviewManager", managerClass)) {
        LogLine("预览：找不到类 A_ClientHPreviewManager（反射链可能已过期）");
        return false;
    }

    uintptr_t manager = 0;
    if (!ue::FindInstanceOfClass(managerClass, "A_ClientHPreviewManager", manager)) {
        ue::FindInstanceOfClass(managerClass, nullptr, manager);
    }

    uintptr_t groupClass = 0;
    ue::FindClassByName("A_ClientHPreviewGroup", groupClass);

    fixup = Fixup{};
    fixup.managerClass = managerClass;
    fixup.manager = manager;
    fixup.groupClass = groupClass;
    fixup.managerSize = ue::PropertiesSize(managerClass);

    const bool ok = ue::FindProperty(managerClass, "preview", fixup.preview) &&
                    ue::FindProperty(managerClass, "isSnappingItem", fixup.snapItem) &&
                    ue::FindProperty(managerClass, "isSnappingWall", fixup.snapWall) &&
                    ue::FindProperty(managerClass, "FloorPreviewZ", fixup.floorZ);

    ue::FindProperty(managerClass, "Z", fixup.zField);
    ue::FindProperty(managerClass, "wallHitLoaction", fixup.hitLoc);
    if (groupClass != 0) {
        ue::FindProperty(groupClass, "Target", fixup.groupTarget);
    }

    if (!ok) {
        LogLine("预览：管理器属性定位失败（preview/item/wall/floor 缺一），功能不可用");
        return false;
    }

    fixup.ready = manager != 0;
    LogLine("预览：manager=0x%llX preview+0x%X item+0x%X wall+0x%X floor+0x%X Z+0x%X hit+0x%X"
            " target+0x%X size=0x%X",
            static_cast<unsigned long long>(manager), fixup.preview.offset, fixup.snapItem.offset,
            fixup.snapWall.offset, fixup.floorZ.offset, fixup.zField.offset, fixup.hitLoc.offset,
            fixup.groupTarget.offset, fixup.managerSize);
    return fixup.ready;
}

bool RefreshFixup(bool force) {
    Fixup current = CopyFixup();
    if (!force && current.ready && ue::IsAlive(current.manager, current.managerClass)) {
        return true;
    }

    // 换场景后旧实例会变成野指针：先作废，扫描期间宁可不写也不要写错地方。
    if (current.ready) {
        StoreFixup(Fixup{});
    }

    Fixup fresh;
    if (!BuildFixup(fresh)) {
        return false;
    }
    StoreFixup(fresh);
    return true;
}

// 预览角色当前的世界坐标：优先用 Group.Target 组件，退回 Group 根组件。
bool ResolveGhostPosition(const Fixup& fixup, uintptr_t group, double (&out)[3]) {
    uintptr_t component = 0;
    if (fixup.groupTarget.valid) {
        component = ReadPtr(group + fixup.groupTarget.offset);
    }
    if (!IsUserPointer(component)) {
        component = ReadPtr(group + ue::kActorRootComponent);
    }
    if (!IsUserPointer(component)) {
        return false;
    }
    return ReadVector(component + ue::kRootWorldLocation, out);
}

int ApplyCore(bool verbose) {
    const Config& cfg = Cfg();
    const Fixup fixup = CopyFixup();

    if (!fixup.ready) {
        return 0;
    }
    if (!ue::IsAlive(fixup.manager, fixup.managerClass)) {
        return 0;
    }

    const uintptr_t manager = fixup.manager;
    const uintptr_t preview = ReadPtr(manager + fixup.preview.offset);
    if (!IsUserPointer(preview)) {
        return 1;  // 不在预览状态：完全不动
    }

    bool snappingItem = false;
    bool snappingWall = false;
    bool floorPreview = false;
    ReadBool(manager, fixup.snapItem, snappingItem);
    ReadBool(manager, fixup.snapWall, snappingWall);
    ReadBool(manager, fixup.floorZ, floorPreview);

    // 游戏自己认为合法时不要插手：改了反而会让正常的落地/贴墙放置走错分支。
    if (cfg.previewOnlyUnsnapped && (snappingItem || snappingWall || floorPreview)) {
        if (verbose) {
            LogLine("预览：当前已有效（item=%d wall=%d floor=%d），跳过", snappingItem ? 1 : 0,
                    snappingWall ? 1 : 0, floorPreview ? 1 : 0);
        }
        return 2;
    }

    double ghost[3] = {};
    if (!ResolveGhostPosition(fixup, preview, ghost)) {
        if (verbose) {
            LogLine("预览：读不到幽灵坐标（Group=0x%llX）", static_cast<unsigned long long>(preview));
        }
        return -1;
    }

    double oldZ = 0.0;
    if (fixup.zField.valid) {
        Read(manager + fixup.zField.offset, oldZ);
    }
    double oldHit[3] = {};
    if (fixup.hitLoc.valid) {
        ReadVector(manager + fixup.hitLoc.offset, oldHit);
    }

    if (cfg.previewLogOnly) {
        LogLine("预览[只记录]：item=%d wall=%d floor=%d Z=%.1f hit=(%.1f,%.1f,%.1f) "
                "ghost=(%.1f,%.1f,%.1f)",
                snappingItem ? 1 : 0, snappingWall ? 1 : 0, floorPreview ? 1 : 0, oldZ, oldHit[0],
                oldHit[1], oldHit[2], ghost[0], ghost[1], ghost[2]);
        return 3;
    }

    int written = 0;
    if (cfg.previewSetSnapWall && WriteBool(manager, fixup.snapWall, true)) {
        ++written;
    }
    if (cfg.previewSetFloor && WriteBool(manager, fixup.floorZ, true)) {
        ++written;
    }
    if (cfg.previewSetZ && WriteDouble(manager, fixup.zField, ghost[2])) {
        ++written;
    }
    if (cfg.previewWriteHitLoc && WriteVector(manager, fixup.hitLoc, ghost)) {
        ++written;
    }

    const long count = InterlockedIncrement(&g_appliedCount);
    LogLine("预览[改写 #%ld]：item=%d wall=%d floor=%d Z %.1f->%.1f hit=(%.1f,%.1f,%.1f)->"
            "(%.1f,%.1f,%.1f) 写入 %d 项",
            count, snappingItem ? 1 : 0, snappingWall ? 1 : 0, floorPreview ? 1 : 0, oldZ, ghost[2],
            oldHit[0], oldHit[1], oldHit[2], ghost[0], ghost[1], ghost[2], written);
    return written > 0 ? 3 : -1;
}

// 左键按下：先同步改写一次（赶在游戏处理这次点击之前），再让后台线程保持一小段时间，
// 避免"这次点击被下一帧的 ReceiveTick 覆盖回去"的竞态。
void OnLeftButtonDown() {
    if (!Cfg().previewFix) {
        return;
    }
    ApplyCore(true);

    const int holdMs = Cfg().previewHoldMs;
    if (holdMs > 0) {
        InterlockedExchange(&g_holdUntil, static_cast<LONG>(::GetTickCount()) + holdMs);
        if (g_wake != nullptr) {
            ::SetEvent(g_wake);
        }
    }
}

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && lParam != 0) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if ((info->flags & LLMHF_INJECTED) == 0 && IsGameForeground()) {
            OnLeftButtonDown();
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// 保持期结束后回读一次：如果三个标志都被游戏改回 0，说明改写被 ReceiveTick 覆盖，
// 需要靠 PreviewHoldMs 抢时序；如果仍为 1，说明状态保住了、剩下的看点击处理函数认不认。
void LogHoldEnd() {
    const Fixup fixup = CopyFixup();
    if (!fixup.ready || !ue::IsAlive(fixup.manager, fixup.managerClass)) {
        return;
    }
    bool item = false;
    bool wall = false;
    bool floor = false;
    ReadBool(fixup.manager, fixup.snapItem, item);
    ReadBool(fixup.manager, fixup.snapWall, wall);
    ReadBool(fixup.manager, fixup.floorZ, floor);
    double z = 0.0;
    if (fixup.zField.valid) {
        Read(fixup.manager + fixup.zField.offset, z);
    }
    LogLine("预览[保持结束]：item=%d wall=%d floor=%d Z=%.1f", item ? 1 : 0, wall ? 1 : 0,
            floor ? 1 : 0, z);
}

DWORD WINAPI WorkerProc(LPVOID) {
    DWORD gap = 500;
    bool holding = false;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        ::WaitForSingleObject(g_wake, 5);
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) {
            break;
        }

        const DWORD now = ::GetTickCount();
        const LONG holdUntil = InterlockedCompareExchange(&g_holdUntil, 0, 0);
        if (static_cast<LONG>(now - holdUntil) < 0) {
            ApplyCore(false);  // 保持期间静默重写
            holding = true;
            continue;
        }
        if (holding) {
            holding = false;
            LogHoldEnd();
        }

        if (now - static_cast<DWORD>(InterlockedCompareExchange(&g_lastRefresh, 0, 0)) >= gap) {
            const bool ok = RefreshFixup(false);
            // 没有管理器（在大厅/菜单）时不要每 2 秒扫一遍 22 万个对象：退避到 15 秒。
            gap = ok ? 500 : (gap < 15000 ? gap * 2 : 15000);
            InterlockedExchange(&g_lastRefresh, static_cast<LONG>(::GetTickCount()));
        }
    }
    return 0;
}

DWORD WINAPI HookThreadProc(LPVOID) {
    g_hookThreadId = ::GetCurrentThreadId();

    MSG msg;
    ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

    g_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_selfModule, 0);
    if (g_mouseHook == nullptr) {
        LogLine("鼠标钩子：SetWindowsHookEx 失败 (%lu)", ::GetLastError());
        return 0;
    }
    LogLine("鼠标钩子：已安装（左键 -> 预览强制落位）");

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g_mouseHook != nullptr) {
        ::UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        LogLine("鼠标钩子：已卸载");
    }
    return 0;
}

}  // namespace

bool StartPreviewBypass(HMODULE hSelf) {
    if (g_hookThread != nullptr) {
        return true;
    }
    if (!Cfg().previewFix) {
        LogLine("预览：PreviewFix=0，功能未启用");
        return true;
    }

    if (!g_lockReady) {
        ::InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    g_selfModule = hSelf;
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_holdUntil, 0);
    InterlockedExchange(&g_lastRefresh, 0);

    if (!ue::InitRuntime()) {
        LogLine("预览：反射初始化失败，功能不可用");
        return false;
    }

    g_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_wake == nullptr) {
        LogLine("预览：CreateEvent 失败 (%lu)", ::GetLastError());
        return false;
    }

    // 先同步解析一次，避免刚注入的几秒内点左键没反应。约几百毫秒，只在安装时一次。
    const DWORD scanStart = ::GetTickCount();
    RefreshFixup(true);
    LogLine("预览：首次解析耗时 %lu ms", ::GetTickCount() - scanStart);

    g_worker = ::CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (g_worker == nullptr) {
        LogLine("预览：工作线程创建失败 (%lu)", ::GetLastError());
        return false;
    }

    g_hookThread = ::CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
    if (g_hookThread == nullptr) {
        LogLine("预览：钩子线程创建失败 (%lu)", ::GetLastError());
        StopPreviewBypass();
        return false;
    }

    for (int i = 0; i < 100; ++i) {
        if (g_mouseHook != nullptr) {
            return true;
        }
        ::Sleep(20);
    }
    LogLine("预览：钩子超时未安装");
    StopPreviewBypass();
    return false;
}

void StopPreviewBypass() {
    if (g_hookThread == nullptr && g_worker == nullptr) {
        return;
    }

    InterlockedExchange(&g_stop, 1);
    if (g_wake != nullptr) {
        ::SetEvent(g_wake);
    }
    if (g_hookThreadId != 0) {
        ::PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    }

    if (g_hookThread != nullptr) {
        ::WaitForSingleObject(g_hookThread, 3000);
        if (g_mouseHook != nullptr) {
            ::UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }
        ::CloseHandle(g_hookThread);
        g_hookThread = nullptr;
        g_hookThreadId = 0;
    }

    if (g_worker != nullptr) {
        ::WaitForSingleObject(g_worker, 3000);
        ::CloseHandle(g_worker);
        g_worker = nullptr;
    }

    if (g_wake != nullptr) {
        ::CloseHandle(g_wake);
        g_wake = nullptr;
    }
}

bool IsPreviewBypassInstalled() {
    return g_mouseHook != nullptr;
}

int PreviewApplyNow() {
    RefreshFixup(false);
    return ApplyCore(true);
}

int PreviewQueryState() {
    RefreshFixup(false);
    const Fixup fixup = CopyFixup();
    if (!fixup.ready || !ue::IsAlive(fixup.manager, fixup.managerClass)) {
        LogLine("预览状态：找不到 A_ClientHPreviewManager 实例");
        return 0;
    }
    const uintptr_t preview = ReadPtr(fixup.manager + fixup.preview.offset);
    if (!IsUserPointer(preview)) {
        LogLine("预览状态：manager=0x%llX，当前不在预览",
                static_cast<unsigned long long>(fixup.manager));
        return 1;
    }
    bool item = false;
    bool wall = false;
    bool floor = false;
    ReadBool(fixup.manager, fixup.snapItem, item);
    ReadBool(fixup.manager, fixup.snapWall, wall);
    ReadBool(fixup.manager, fixup.floorZ, floor);
    double z = 0.0;
    if (fixup.zField.valid) {
        Read(fixup.manager + fixup.zField.offset, z);
    }
    double ghost[3] = {};
    ResolveGhostPosition(fixup, preview, ghost);
    LogLine("预览状态：manager=0x%llX group=0x%llX item=%d wall=%d floor=%d Z=%.1f "
            "ghost=(%.1f,%.1f,%.1f)",
            static_cast<unsigned long long>(fixup.manager), static_cast<unsigned long long>(preview),
            item ? 1 : 0, wall ? 1 : 0, floor ? 1 : 0, z, ghost[0], ghost[1], ghost[2]);
    return 2;
}

}  // namespace ge
