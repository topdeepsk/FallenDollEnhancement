#include "Dll/ScriptTrace.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"
#include "Dll/Config.h"
#include "Dll/UeRuntime.h"

namespace ge {

namespace {

using ProcessEventFn = void (*)(void* self, void* function, void* parms);

ProcessEventFn g_original = nullptr;
bool g_retryQuiet = false;   // 重试期间静音，避免每 3 秒刷一条失败日志
void* g_originalValue = nullptr;
void** g_patchedSlot = nullptr;
uintptr_t g_vtable = 0;
int g_slotOffset = -1;
bool g_installed = false;

// --- 补提交所需的运行时地址 ---
uintptr_t g_managerClass = 0;      // A_ClientHPreviewManager
uintptr_t g_clientHClass = 0;      // A_ClientHManager
uintptr_t g_checkFn = 0;           // OnHCheckPlayHCard
uintptr_t g_prePlayFn = 0;         // OnHPrePlayHCard
volatile uintptr_t g_clientHManager = 0;  // 见到的 A_ClientHManager 实例（游戏线程写）
volatile LONG g_sawCommit = 0;     // 本次点击里是否已经走到提交
volatile LONG g_pendingCommit = 0; // CLI 排队的强制提交
volatile uintptr_t g_previewManager = 0;  // 最近一次左键点击所在的预览管理器实例
uintptr_t g_previewClass = 0;
uint32_t g_previewOffset = 0;
uint32_t g_cardDataOffset = 0;  // A_ClientHPreviewManager::CardData（StructProperty）
uint32_t g_cardDataClass = 0;

// ---------------------------------------------------------------------------
// 调用捕获
//
// 逐条打印会把日志刷爆（光是 UI 动画每秒就有几十条），所以改成：检测到左键处理函数
// 被调用时，把接下来 500ms 内的调用记进一个 64 条的缓冲区，之后用 GE_TraceDump 导出。
// 两个缓冲区轮换，这样可以连着抓"绿色点击"和"红色点击"再一起看。
// ---------------------------------------------------------------------------
constexpr int kCaptureCapacity = 128;
constexpr DWORD kCaptureWindowMs = 2000;

// 每帧都会刷的、和放置无关的调用：不过滤的话 128 条会在 1ms 内被填满，
// 真正发生在几十毫秒后的提交调用就被挤掉了（本项目踩过这个坑）。
bool IsNoise(const std::string& name) {
    static const char* kNoise[] = {
        "ReceiveTick", "BndEvt__", "OnAudioMouth", "OnAudioSingleEnvelopeValue",
        "TickAutoDOF", "Optimization", "CheckDecal", "CheckHOverlap",
        "InpAxisKeyEvt_MouseWheelAxis", "UserConstructionScript",
    };
    for (const char* token : kNoise) {
        if (name.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 蓝图输入事件在 FName 里带编号（InpActEvt_LeftMouseButton_..._2 / _13），
// 精确比较永远匹配不上，所以按前缀判断，再用宿主类名确认是预览管理器那一份。
bool IsClickTrigger(const std::string& name, const std::string& owner) {
    if (owner != "A_ClientHPreviewManager") {
        return false;
    }
    return name.rfind("InpActEvt_LeftMouseButton_K2Node_InputKeyEvent", 0) == 0 ||
           name.rfind("InpActEvt_IA_Click_Trigger_Left_K2Node_EnhancedInputActionEvent", 0) == 0;
}

struct TraceEntry {
    char  text[96] = {};
    DWORD tick = 0;
};

TraceEntry g_capture[2][kCaptureCapacity];
int g_captureCount[2] = {0, 0};
DWORD g_captureStart[2] = {0, 0};
int g_captureSlot = 0;          // 当前写入哪个缓冲区
volatile LONG g_capturing = 0;  // 非 0 表示正在抓
DWORD g_captureUntil = 0;
int g_captureWriteSlot = 0;

void Append(const char* text, DWORD tick) {
    const int slot = g_captureWriteSlot;
    if (g_captureCount[slot] >= kCaptureCapacity) {
        return;
    }
    TraceEntry& entry = g_capture[slot][g_captureCount[slot]++];
    _snprintf_s(entry.text, sizeof(entry.text), _TRUNCATE, "%s", text);
    entry.tick = tick;
}

void RecordCall(void* self, void* function, void* parms) {
    if (function == nullptr) {
        return;
    }

    const std::string name = ue::ObjectName(reinterpret_cast<uintptr_t>(function));
    if (name.empty()) {
        return;
    }

    // 顺手缓存两样东西：A_ClientHManager 的实例（补提交要往它身上调），
    // 以及"本次点击已经走到提交"的标志（绿色点击里 OnHCheckPlayHCard 会出现）。
    if (g_clientHClass != 0 && self != nullptr) {
        uintptr_t cls = 0;
        uint32_t index = 0;
        uint32_t number = 0;
        uintptr_t outer = 0;
        if (ue::ReadObjectHeader(reinterpret_cast<uintptr_t>(self), cls, index, number, outer) &&
            cls == g_clientHClass) {
            g_clientHManager = reinterpret_cast<uintptr_t>(self);
        }
    }
    if (name == "OnHCheckPlayHCard") {
        InterlockedExchange(&g_sawCommit, 1);
    }

    const DWORD now = ::GetTickCount();

    // 先处理超时：即使这段时间只有噪声调用，窗口也要能正常关闭，
    // 否则 g_capturing 一直为 1，下一次点击就起不了新缓冲。
    if (InterlockedCompareExchange(&g_capturing, 0, 0) != 0 && now > g_captureUntil) {
        InterlockedExchange(&g_capturing, 0);
        LogLine("脚本追踪：捕获结束（缓冲 %d，%d 条）", g_captureWriteSlot,
                g_captureCount[g_captureWriteSlot]);
    }

    const bool capturing = InterlockedCompareExchange(&g_capturing, 0, 0) != 0;

    if (!capturing) {
        // 只在"没在抓"的时候判断是不是点击，避免 KeyEvent 与 EnhancedInput 两路
        // 同时触发时把缓冲区来回切换。
        const std::string owner =
            self != nullptr ? ue::ClassNameOf(reinterpret_cast<uintptr_t>(self)) : std::string();
        if (!IsClickTrigger(name, owner)) {
            return;
        }
        g_captureWriteSlot = (g_captureWriteSlot + 1) & 1;
        g_captureCount[g_captureWriteSlot] = 0;
        g_captureStart[g_captureWriteSlot] = now;
        g_captureUntil = now + kCaptureWindowMs;
        InterlockedExchange(&g_capturing, 1);
        LogLine("脚本追踪：捕获开始（缓冲 %d）", g_captureWriteSlot);
    }

    if (IsNoise(name)) {
        return;
    }

    const std::string owner =
        self != nullptr ? ue::ClassNameOf(reinterpret_cast<uintptr_t>(self)) : std::string();
    char line[96] = {};
    if (parms != nullptr && (name.find("OnHCheckPlayHCard") != std::string::npos ||
                             name.find("OnHPrePlayHCard") != std::string::npos)) {
        uint32_t value = 0;
        ReadMemory(reinterpret_cast<uintptr_t>(parms), &value, sizeof(value));
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s::%s p=0x%X", owner.c_str(), name.c_str(),
                    value);
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s::%s", owner.c_str(), name.c_str());
    }
    Append(line, now);

    if (g_captureCount[g_captureWriteSlot] >= kCaptureCapacity) {
        InterlockedExchange(&g_capturing, 0);
        LogLine("脚本追踪：捕获结束（缓冲 %d 已满）", g_captureWriteSlot);
    }
}

bool IsPreviewActive(void* self) {
    if (self == nullptr) {
        return false;
    }
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!ue::ReadObjectHeader(reinterpret_cast<uintptr_t>(self), cls, index, number, outer)) {
        return false;
    }
    if (cls != g_previewClass || g_previewOffset == 0) {
        ue::PropInfo prop;
        if (!ue::FindProperty(cls, "preview", prop)) {
            return false;
        }
        g_previewClass = cls;
        g_previewOffset = prop.offset;
    }
    uintptr_t group = 0;
    if (!Read(reinterpret_cast<uintptr_t>(self) + g_previewOffset, group)) {
        return false;
    }
    return IsUserPointer(group);
}

// 在游戏线程上补齐游戏本来会做的两步提交。
// 只在左键处理函数跑完、且它没有走到 OnHCheckPlayHCard 时调用。
void ExecCommit(const char* reason) {    if (g_original == nullptr || g_checkFn == 0 || g_prePlayFn == 0) {
        LogLine("补提交(%s)：提交函数未解析，跳过", reason);
        return;
    }
    uintptr_t manager = g_clientHManager;
    if (manager == 0 || !ue::IsAlive(manager, g_clientHClass)) {
        // 换场景后实例会变。这里在游戏线程上重扫一次，2 秒内只允许一次，
        // 避免连点把每帧都拖慢几百毫秒。
        static DWORD lastScan = 0;
        const DWORD now = ::GetTickCount();
        if (now - lastScan >= 2000) {
            lastScan = now;
            uintptr_t found = 0;
            if (!ue::FindInstanceOfClass(g_clientHClass, "A_ClientHManager", found)) {
                ue::FindInstanceOfClass(g_clientHClass, nullptr, found);
            }
            if (found != 0) {
                g_clientHManager = found;
                manager = found;
            }
        }
    }
    if (manager == 0 || !ue::IsAlive(manager, g_clientHClass)) {
        LogLine("补提交(%s)：还没有见到 A_ClientHManager 实例，跳过", reason);
        return;
    }

    // 提交函数实际会从参数缓冲里读一个 32 位卡 ID（绿色点击抓到的是 0x1DD，
    // 正好等于 CardData 结构 +8 处的值）。传 0 会拿无效卡去查，反而更危险。
    uint32_t cardId = 0;
    const uintptr_t previewManager = g_previewManager;
    if (previewManager != 0 && g_cardDataOffset != 0) {
        Read(previewManager + g_cardDataOffset + 8, cardId);
    }
    if (cardId == 0) {
        LogLine("补提交(%s)：读不到卡 ID（manager=0x%llX cardData+0x%X），跳过",
                reason, static_cast<unsigned long long>(previewManager), g_cardDataOffset);
        return;
    }

    alignas(16) uint8_t frame[256] = {};
    memcpy(frame, &cardId, sizeof(cardId));

    LogLine("补提交(%s)：cardId=0x%X manager=0x%llX 调用 OnHCheckPlayHCard", reason, cardId,
            static_cast<unsigned long long>(manager));

    // 参数缓冲不能传 nullptr：引擎会拿它当返回值基址，脚本函数读写时报
    // EXCEPTION_ACCESS_VIOLATION reading 0x0（实测）。
    g_original(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(g_checkFn), frame);

    LogLine("补提交(%s)：OnHCheckPlayHCard 返回，调用 OnHPrePlayHCard", reason);
    g_original(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(g_prePlayFn), frame);

    LogLine("补提交(%s)：两步都已返回", reason);
}

void __cdecl ProcessEventThunk(void* self, void* function, void* parms) {
    // CLI 排队的强制提交：必须在游戏线程（也就是这里）执行，
    // 从注入器线程直接调 ProcessEvent 会和游戏线程死锁（已实测）。
    if (InterlockedExchange(&g_pendingCommit, 0) != 0) {
        ExecCommit("CLI");
    }

    // 判断是不是管理器那一份左键处理函数：前缀匹配 + 宿主类确认。
    bool isLeftClick = false;
    if (function != nullptr && self != nullptr && g_managerClass != 0) {
        uintptr_t cls = 0;
        uint32_t index = 0;
        uint32_t number = 0;
        uintptr_t outer = 0;
        if (ue::ReadObjectHeader(reinterpret_cast<uintptr_t>(self), cls, index, number, outer) &&
            cls == g_managerClass) {
            const std::string name = ue::ObjectName(reinterpret_cast<uintptr_t>(function));
            isLeftClick = name.rfind("InpActEvt_LeftMouseButton_K2Node_InputKeyEvent", 0) == 0 ||
                          name.rfind("InpActEvt_IA_Click_Trigger_Left", 0) == 0;
        }
    }
    if (isLeftClick) {
        InterlockedExchange(&g_sawCommit, 0);
        g_previewManager = reinterpret_cast<uintptr_t>(self);
    }

    // RecordCall 内部全部走 ReadProcessMemory（不会抛访问违例），这里不再套 SEH：
    // 混用 SEH 与带析构对象的 C++ 函数属于未定义行为，收益也几乎没有。
    RecordCall(self, function, parms);
    if (g_original != nullptr) {
        g_original(self, function, parms);
    }

    // 原处理函数返回后：如果本次点击没有走到提交，且当前确实在预览状态，就补上。
    if (isLeftClick && Cfg().previewAutoCommit &&
        InterlockedCompareExchange(&g_sawCommit, 0, 0) == 0 && IsPreviewActive(self)) {
        ExecCommit("左键判定未通过");
    }
}

// ProcessEvent 的特征：`test dword ptr [<UFunction>+0xB0], imm32` 且 imm32 含 0x400（FUNC_Native）。
//
// 不能按 DUMPER-7 原签名要求立即数恰为 0x400：本作把 FUNC_Native 与 FUNC_Public 合并成
// 一条 test，立即数是 0x410。反而"位移必须是 0xB0"（UFunction::FunctionFlags）更可靠——
// 只按立即数判断会误命中 `test [reg+0xD4], 0x400` 这类无关函数（本项目踩过：
// 误命中 vtable+0x1E0，把无关虚函数换成了 thunk）。
bool LooksLikeProcessEvent(uintptr_t function) {
    if (function == 0) {
        return false;
    }
    uint8_t body[0x800] = {};
    if (!ReadMemory(function, body, sizeof(body))) {
        return false;
    }
    for (size_t i = 0; i + 10 <= sizeof(body); ++i) {
        if (body[i] != 0xF7) {
            continue;
        }
        if (body[i + 2] != 0xB0 || body[i + 3] != 0 || body[i + 4] != 0 || body[i + 5] != 0) {
            continue;
        }
        const uint32_t imm = static_cast<uint32_t>(body[i + 6]) |
                             (static_cast<uint32_t>(body[i + 7]) << 8) |
                             (static_cast<uint32_t>(body[i + 8]) << 16) |
                             (static_cast<uint32_t>(body[i + 9]) << 24);
        if ((imm & 0x400u) != 0) {
            return true;
        }
    }
    return false;
}

bool CollectBaseVTables(uintptr_t (&out)[3]) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    uintptr_t world = 0;
    if (!Read(base + Cfg().gworldRva, world) || !IsUserPointer(world)) {
        return false;
    }
    uintptr_t gameInstance = 0;
    if (!Read(world + ue::kWorldOwningGameInstance, gameInstance) || !IsUserPointer(gameInstance)) {
        return false;
    }
    uintptr_t players = 0;
    if (!Read(gameInstance + ue::kGameInstanceLocalPlayers, players) || !IsUserPointer(players)) {
        return false;
    }
    uintptr_t localPlayer = 0;
    if (!Read(players, localPlayer) || !IsUserPointer(localPlayer)) {
        return false;
    }

    uintptr_t vtables[3] = {0, 0, 0};
    if (!Read(world, vtables[0]) || !Read(gameInstance, vtables[1]) ||
        !Read(localPlayer, vtables[2])) {
        return false;
    }
    for (uintptr_t vt : vtables) {
        if (!IsUserPointer(vt)) {
            return false;
        }
    }
    out[0] = vtables[0];
    out[1] = vtables[1];
    out[2] = vtables[2];
    return true;
}

bool ResolveSlot(int& outOffset, uintptr_t& outBaseFunction) {
    uintptr_t vtables[3] = {};
    if (!CollectBaseVTables(vtables)) {
        LogLine("脚本追踪：取 UWorld/GameInstance/LocalPlayer 的 vtable 失败");
        return false;
    }

    auto check = [&](int offset) -> bool {
        uintptr_t a = 0;
        uintptr_t b = 0;
        uintptr_t c = 0;
        if (!Read(vtables[0] + static_cast<uintptr_t>(offset), a) ||
            !Read(vtables[1] + static_cast<uintptr_t>(offset), b) ||
            !Read(vtables[2] + static_cast<uintptr_t>(offset), c)) {
            return false;
        }
        if (a == 0 || a != b || a != c || !LooksLikeProcessEvent(a)) {
            return false;
        }
        outOffset = offset;
        outBaseFunction = a;
        return true;
    };

    const int preferred = static_cast<int>(Cfg().processEventSlot);
    if (preferred >= 0x80 && preferred <= 0x800 && check(preferred)) {
        return true;
    }
    for (int offset = 0x100; offset <= 0x300; offset += 8) {
        if (check(offset)) {
            return true;
        }
    }
    LogLine("脚本追踪：vtable 0x%llX / 0x%llX / 0x%llX 里没找到 ProcessEvent",
            static_cast<unsigned long long>(vtables[0]),
            static_cast<unsigned long long>(vtables[1]),
            static_cast<unsigned long long>(vtables[2]));
    return false;
}

bool PatchSlot(void** slot, void* replacement) {
    DWORD oldProtect = 0;
    if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect) == FALSE) {
        return false;
    }
    *slot = replacement;
    DWORD ignored = 0;
    ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

}  // namespace

bool StartScriptTrace() {
    if (g_installed) {
        return true;
    }
    if (!Cfg().traceScript) {
        LogLine("脚本追踪：TraceScript=0，未启用");
        return true;
    }
    if (!ue::InitRuntime()) {
        return false;
    }

    uintptr_t managerClass = 0;
    if (!ue::FindClassByName("A_ClientHPreviewManager", managerClass)) {
        if (!g_retryQuiet) {
            LogLine("脚本追踪：找不到 A_ClientHPreviewManager");
        }
        return false;
    }
    g_managerClass = managerClass;

    // 卡 ID 在 CardData 结构 +8 处；补提交时要把它塞进参数缓冲。
    {
        ue::PropInfo cardProp;
        if (ue::FindProperty(managerClass, "CardData", cardProp)) {
            g_cardDataOffset = cardProp.offset;
        }
        LogLine("脚本追踪：CardData+0x%X", g_cardDataOffset);
    }

    // 补提交需要 A_ClientHManager 的两个函数，装的时候就解析好，
    // 避免在 ProcessEvent 热路径里做全表扫描（那是几百毫秒）。
    g_clientHClass = 0;
    g_checkFn = 0;
    g_prePlayFn = 0;
    if (ue::FindClassByName("A_ClientHManager", g_clientHClass)) {
        const bool okCheck = ue::FindFunctionOfClass(g_clientHClass, "OnHCheckPlayHCard", g_checkFn);
        const bool okPre = ue::FindFunctionOfClass(g_clientHClass, "OnHPrePlayHCard", g_prePlayFn);

        // 实例直接在这里解析：红色点击时它不会产生任何脚本调用，
        // 靠"见到再缓存"是等不到的（本项目踩过这个坑）。
        uintptr_t clientManager = 0;
        if (!ue::FindInstanceOfClass(g_clientHClass, "A_ClientHManager", clientManager)) {
            ue::FindInstanceOfClass(g_clientHClass, nullptr, clientManager);
        }
        if (clientManager != 0) {
            g_clientHManager = clientManager;
        }

        LogLine("脚本追踪：提交函数 check=0x%llX(%d) pre=0x%llX(%d) manager=0x%llX",
                static_cast<unsigned long long>(g_checkFn), okCheck ? 1 : 0,
                static_cast<unsigned long long>(g_prePlayFn), okPre ? 1 : 0,
                static_cast<unsigned long long>(clientManager));
    } else {
        LogLine("脚本追踪：找不到 A_ClientHManager，补提交不可用（仅记录调用）");
    }

    uintptr_t instance = 0;
    if (!ue::FindInstanceOfClass(managerClass, "A_ClientHPreviewManager", instance)) {
        ue::FindInstanceOfClass(managerClass, nullptr, instance);
    }
    if (instance == 0) {
        if (!g_retryQuiet) {
            LogLine("脚本追踪：没有管理器实例，无法取 vtable");
        }
        return false;
    }

    uintptr_t vtable = 0;
    if (!Read(instance, vtable) || vtable == 0) {
        LogLine("脚本追踪：读 vtable 失败");
        return false;
    }

    int offset = -1;
    uintptr_t baseFunction = 0;
    if (!ResolveSlot(offset, baseFunction)) {
        return false;
    }

    if (Cfg().traceDryRun) {
        LogLine("脚本追踪[试运行]：定位到 slot=+0x%X 基类实现=0x%llX，未写入 vtable", offset,
                static_cast<unsigned long long>(baseFunction));
        return true;
    }

    void** slot = reinterpret_cast<void**>(vtable + static_cast<uintptr_t>(offset));
    void* original = *slot;
    if (original == nullptr) {
        LogLine("脚本追踪：slot=+0x%X 为空，放弃", offset);
        return false;
    }
    if (!PatchSlot(slot, reinterpret_cast<void*>(&ProcessEventThunk))) {
        LogLine("脚本追踪：vtable 写入失败（页不可写）");
        return false;
    }

    g_originalValue = original;
    g_original = reinterpret_cast<ProcessEventFn>(original);
    g_patchedSlot = slot;
    g_vtable = vtable;
    g_slotOffset = offset;
    g_installed = true;

    LogLine("脚本追踪：接管成功 manager vtable=0x%llX slot=+0x%X 原函数=0x%llX（基类 0x%llX%s）",
            static_cast<unsigned long long>(vtable), offset,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(original)),
            static_cast<unsigned long long>(baseFunction),
            original == reinterpret_cast<void*>(baseFunction) ? "" : "，Actor 覆盖版");
    return true;
}

void StopScriptTrace() {
    if (!g_installed) {
        return;
    }
    if (g_patchedSlot != nullptr && g_originalValue != nullptr) {
        PatchSlot(g_patchedSlot, g_originalValue);
    }
    g_installed = false;
    g_patchedSlot = nullptr;
    g_original = nullptr;
    g_originalValue = nullptr;
    LogLine("脚本追踪：已还原 vtable slot");
}

bool IsScriptTraceInstalled() {
    return g_installed;
}

int ScriptTraceQuery() {
    LogLine("脚本追踪：vtable=0x%llX slot=+0x%X installed=%d 缓冲0=%d 条 缓冲1=%d 条",
            static_cast<unsigned long long>(g_vtable), g_slotOffset, g_installed ? 1 : 0,
            g_captureCount[0], g_captureCount[1]);
    return g_installed ? g_slotOffset : -1;
}

void ScriptTraceDumpCaptures() {
    static const char* kNames[2] = {"缓冲 A", "缓冲 B"};
    for (int slot = 0; slot < 2; ++slot) {
        if (g_captureCount[slot] == 0) {
            continue;
        }
        LogLine("脚本追踪：===== %s（点击时刻 %lu，%d 条）=====", kNames[slot],
                static_cast<unsigned long>(g_captureStart[slot]), g_captureCount[slot]);
        for (int i = 0; i < g_captureCount[slot]; ++i) {
            const TraceEntry& entry = g_capture[slot][i];
            LogLine("  +%4lu ms  %s", static_cast<unsigned long>(entry.tick - g_captureStart[slot]),
                    entry.text);
        }
    }
}

int ScriptTraceForceCommit() {
    if (!g_installed || g_original == nullptr) {
        LogLine("强制提交：脚本追踪未接管，无法调用");
        return -1;
    }
    if (g_checkFn == 0 || g_prePlayFn == 0) {
        LogLine("强制提交：提交函数未解析（需要游戏里有一个 A_ClientHManager）");
        return -2;
    }
    // 不能在注入器线程直接调 ProcessEvent：会和游戏线程死锁（实测超时）。
    // 排队，由游戏线程上的 ProcessEvent thunk 执行。
    InterlockedExchange(&g_pendingCommit, 1);
    LogLine("强制提交：已排队，等游戏线程下一帧执行");
    return 0;
}

namespace {

HANDLE g_retryThread = nullptr;
volatile LONG g_retryStop = 0;

DWORD WINAPI RetryProc(LPVOID) {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (InterlockedCompareExchange(&g_retryStop, 0, 0) != 0) {
            break;
        }
        if (StartScriptTrace() && IsScriptTraceInstalled()) {
            g_retryQuiet = false;
            LogLine("脚本追踪：第 %d 次尝试接管成功", attempt);
            break;
        }
        g_retryQuiet = true;
        ::Sleep(3000);
    }
    g_retryQuiet = false;
    return 0;
}

}  // namespace

void StartScriptTraceRetry() {
    if (g_retryThread != nullptr) {
        return;
    }
    InterlockedExchange(&g_retryStop, 0);
    g_retryThread = ::CreateThread(nullptr, 0, RetryProc, nullptr, 0, nullptr);
    if (g_retryThread == nullptr) {
        LogLine("脚本追踪：重试线程创建失败（%lu）", ::GetLastError());
    }
}

void StopScriptTraceRetry() {
    if (g_retryThread == nullptr) {
        return;
    }
    InterlockedExchange(&g_retryStop, 1);
    ::WaitForSingleObject(g_retryThread, 8000);
    ::CloseHandle(g_retryThread);
    g_retryThread = nullptr;
}

}  // namespace ge
