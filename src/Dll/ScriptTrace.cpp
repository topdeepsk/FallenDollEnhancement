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
volatile LONG g_pendingCall = 0;   // CLI 排队的任意函数调用（实验用）
char g_callClass[64] = {};
char g_callFunc[128] = {};

// --- 恢复玩家显示与控制（P 键）---
volatile LONG g_pendingRestore = 0;   // 键盘钩子只落这个标志，实际执行在游戏线程
volatile LONG g_pendingQuitExp = 0;   // O 键：退出流程 + 拦销毁实验
volatile LONG g_pendingCamSwitch = 0; // 空格：转发给普通模式下的切镜头处理函数
char g_probeSeen[24][96] = {};       // 输入探针：已记录过的输入函数名
int g_probeCount = 0;
volatile LONG g_lookDx = 0;          // 鼠标位移累加（钩子写，游戏线程取）
volatile LONG g_lookDy = 0;
volatile LONG g_lastLookApply = 0;
uintptr_t g_fnAddYaw = 0;
uintptr_t g_fnAddPitch = 0;

// --- 销毁拦截：换 UFunction::Func 指针（数据写入，不动代码段）---
using NativeFunc3 = void (*)(void*, void*, void*);
NativeFunc3 g_origK2Destroy = nullptr;
void* g_k2DestroySlot = nullptr;
volatile LONG g_destroyBlocked = 0;   // 累计拦住多少次，便于日志确认
volatile LONG g_restoreUntil = 0;     // 这段时间内每帧补一次"取消隐藏"，压过游戏的隐藏逻辑
volatile LONG g_lastRestoreApply = 0;
uintptr_t g_pawnModeClass = 0;
uintptr_t g_actorClass = 0;
uintptr_t g_controllerClass = 0;
uintptr_t g_fnSetHidden = 0;
uintptr_t g_fnPossess = 0;
uintptr_t g_fnEnableInput = 0;
uintptr_t g_fnDisableInput = 0;
uintptr_t g_fnSetViewTarget = 0;
uint32_t g_avatarOffset = 0;
uint32_t g_freeCamOffset = 0;
volatile LONG g_restoreActive = 0;  // P 键切换：0 = H 模式，1 = 玩家角色模式
uint32_t g_inputCompOffset = 0;     // AActor::InputComponent
uintptr_t g_hInputComps[4] = {};    // A_ClientHController 上的输入组件（HInputComp / HEnhancedInputComp）
int g_hInputCompCount = -1;         // -1 = 还没解析
uint32_t g_performerOffset = 0;     // A_ClientHController::MyHPerformer（非空 = 正在播放）
uintptr_t g_fnOnEscPressed = 0;
volatile LONG g_suppressUntil = 0;  // 抑制窗口：这段时间内拦住"停止播放"的调用

// 退出流程里会真正结束动作的那些调用（子串匹配）。
// 只拦它们，其余（切 pawn、恢复镜头、取消隐藏）照常执行。
// 只拦一个：把"停止播放"这一步掐掉，其余（ClientChangeState、销毁等）全部照常执行，
// 用来确认单纯阻止这一处能否保住动作。
const char* const kStopTokens[] = {
    "OnStopHPerformerPlaying",
};

bool Suppressing() {
    const LONG until = InterlockedCompareExchange(&g_suppressUntil, 0, 0);
    if (until == 0) {
        return false;
    }
    if (static_cast<LONG>(::GetTickCount() - static_cast<DWORD>(until)) >= 0) {
        return false;
    }
    return true;
}

bool FindFunctionInChain(uintptr_t cls, const char* name, uintptr_t& outFunc);

// K2_DestroyActor 的替身：**只记录，不阻止**。
// 曾经试过直接 return 不销毁，结果调用方在等"对象已失效"，游戏线程原地死循环卡死（实测）。
// 所以这里永远转给原函数，只在抑制窗口内把"是谁被销毁"打进日志。
void __cdecl K2DestroyThunk(void* self, void* frame, void* result) {
    if (Suppressing() && self != nullptr) {
        const std::string cls = ue::ClassNameOf(reinterpret_cast<uintptr_t>(self));
        const std::string name = ue::ObjectName(reinterpret_cast<uintptr_t>(self));
        InterlockedIncrement(&g_destroyBlocked);
        LogLine("销毁追踪：%s / %s", cls.c_str(), name.c_str());
    }
    if (g_origK2Destroy != nullptr) {
        g_origK2Destroy(self, frame, result);
    } else if (frame != nullptr) {
        // 兜底：原函数丢了也必须让引擎自己走完，否则就是卡死。
        LogLine("销毁追踪：原函数为空，跳过（可能出现对象泄漏）");
    }
}

// 把 Actor::K2_DestroyActor 的 Func 换成上面的 thunk。
bool InstallDestroyHook() {
    if (g_k2DestroySlot != nullptr) {
        return true;
    }
    uintptr_t actorClass = 0;
    if (!ue::FindClassByName("Actor", actorClass)) {
        LogLine("销毁钩子：找不到 Actor 类");
        return false;
    }
    uintptr_t fn = 0;
    if (!FindFunctionInChain(actorClass, "K2_DestroyActor", fn)) {
        LogLine("销毁钩子：找不到 K2_DestroyActor");
        return false;
    }
    void* slot = reinterpret_cast<void*>(fn + 0xD8);
    void* original = nullptr;
    if (!Read(reinterpret_cast<uintptr_t>(slot), original) || original == nullptr) {
        LogLine("销毁钩子：读 Func 失败");
        return false;
    }
    g_origK2Destroy = reinterpret_cast<NativeFunc3>(original);
    if (!Write(reinterpret_cast<uintptr_t>(slot),
               reinterpret_cast<void*>(&K2DestroyThunk))) {
        LogLine("销毁钩子：写 Func 失败");
        g_origK2Destroy = nullptr;
        return false;
    }
    g_k2DestroySlot = slot;
    LogLine("销毁钩子：已接管 K2_DestroyActor（原函数 0x%llX）",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(original)));
    return true;
}

bool IsStopPlaybackCall(const std::string& name) {
    for (const char* token : kStopTokens) {
        if (name.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}
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
constexpr int kCaptureCapacity = 384;
constexpr DWORD kCaptureWindowMs = 3000;

// 每帧都会刷的、和放置无关的调用：不过滤的话 128 条会在 1ms 内被填满，
// 真正发生在几十毫秒后的提交调用就被挤掉了（本项目踩过这个坑）。
bool IsNoise(const std::string& name) {
    static const char* kNoise[] = {
        "ReceiveTick", "BndEvt__", "OnAudioMouth", "OnAudioSingleEnvelopeValue",
        "TickAutoDOF", "Optimization", "CheckDecal", "CheckHOverlap",
        "InpAxisKeyEvt_MouseWheelAxis", "UserConstructionScript",
        // 播放期间每帧都在刷的动画/镜头驱动，会把缓冲区瞬间填满（实测：128 条只用 90ms）。
        "OnTimeline_", "InpAxisEvt_", "InpAxisKeyEvt_Mouse", "InpAxisKeyEvt_Gamepad",
        "Fade__UpdateFunc", "Timeline_",
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

// ESC 的捕获触发：动作播放期间的处理者不是预览管理器（A_ClientHController），
// 所以这里不限定宿主类，只认函数名——EnhancedInput 的 Menu_Close 两路 + 任何带 Esc 的处理。
bool IsEscTrigger(const std::string& name) {
    return name.rfind("InpActEvt_IA_Menu_Close", 0) == 0 ||
           name.find("Esc") != std::string::npos;
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

    // 输入探针：只在"玩家角色模式"下工作，记录还有哪些输入函数在被调用。
    // 用途是定位"谁还在吃 WASD"——H 的输入组件可能不止一个，或另有宿主。
    if ((InterlockedCompareExchange(&g_restoreActive, 0, 0) != 0 || Cfg().probeInput) &&
        name.rfind("Inp", 0) == 0) {
        if (g_probeCount >= 24) {
            g_probeCount = 0;  // 滚动窗口：满了就重来，保证最近的按键一定能被记到
        }
        bool duplicated = false;
        for (int i = 0; i < g_probeCount; ++i) {
            if (_stricmp(g_probeSeen[i], name.c_str()) == 0) {
                duplicated = true;
                break;
            }
        }
        if (!duplicated) {
            strncpy_s(g_probeSeen[g_probeCount], name.c_str(), _TRUNCATE);
            ++g_probeCount;
            const std::string probeOwner =
                self != nullptr ? ue::ClassNameOf(reinterpret_cast<uintptr_t>(self)) : std::string();
            LogLine("输入探针：%s::%s", probeOwner.c_str(), name.c_str());
        }
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
    // 播放开始/结束/退出：把"玩家角色模式"标志复位，保证 P 键判断不与游戏状态失同步。
    if (name == "OnHPerformerPlaying" || name == "OnStopHPerformerPlaying" ||
        name == "OnQuitHState") {
        InterlockedExchange(&g_restoreActive, 0);
        InterlockedExchange(&g_restoreUntil, 0);
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
        const bool click = IsClickTrigger(name, owner);
        if (!click && !IsEscTrigger(name)) {
            return;
        }
        g_captureWriteSlot = (g_captureWriteSlot + 1) & 1;
        g_captureCount[g_captureWriteSlot] = 0;
        g_captureStart[g_captureWriteSlot] = now;
        g_captureUntil = now + kCaptureWindowMs;
        InterlockedExchange(&g_capturing, 1);
        LogLine("脚本追踪：捕获开始（缓冲 %d，触发=%s）", g_captureWriteSlot, click ? "左键" : "ESC");
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

// 按前缀找函数对象：蓝图输入事件在 FName 里带编号（_2 / _13），精确匹配必然失败。
struct PrefixFind {
    uintptr_t cls;
    const char* prefix;
    uintptr_t found;
};

bool VisitFunctionPrefix(uintptr_t object, void* user) {
    auto* find = static_cast<PrefixFind*>(user);
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!ue::ReadObjectHeader(object, cls, index, number, outer) || outer != find->cls) {
        return true;
    }
    const std::string name = ue::ObjectName(object);
    if (name.rfind(find->prefix, 0) == 0) {
        find->found = object;
        return false;
    }
    return true;
}

uintptr_t FindFunctionByPrefix(uintptr_t cls, const char* prefix) {
    if (cls == 0 || prefix == nullptr) {
        return 0;
    }
    PrefixFind find{cls, prefix, 0};
    ue::ScanObjects(&VisitFunctionPrefix, &find);
    return find.found;
}

// 沿父类链找函数对象：属性/函数常常声明在基类上，子类实例照样能调用。
bool FindFunctionInChain(uintptr_t cls, const char* name, uintptr_t& outFunc) {
    uintptr_t c = cls;
    for (int depth = 0; c != 0 && depth < 8; ++depth) {
        if (ue::FindFunctionOfClass(c, name, outFunc)) {
            return true;
        }
        uintptr_t super = 0;
        if (!Read(c + ue::kUStructSuper, super)) {
            return false;
        }
        c = super;
    }
    return false;
}

// 实验入口：按名字调一次游戏自己的函数（无参或参数全 0）。
// 必须在游戏线程执行，所以由 thunk 排队调用，CLI 只落标志。
void ExecCall() {
    const std::string clsName = g_callClass;
    const std::string funcName = g_callFunc;
    if (clsName.empty() || funcName.empty()) {
        LogLine("调用实验：缺少类名/函数名");
        return;
    }

    uintptr_t cls = 0;
    uintptr_t inst = 0;
    uintptr_t fn = 0;
    if (!ue::FindClassByName(clsName.c_str(), cls)) {
        LogLine("调用实验：找不到类 %s", clsName.c_str());
        return;
    }
    if (!ue::FindInstanceOfClass(cls, nullptr, inst)) {
        LogLine("调用实验：%s 没有实例", clsName.c_str());
        return;
    }
    if (!FindFunctionInChain(cls, funcName.c_str(), fn)) {
        LogLine("调用实验：%s 上找不到函数 %s", clsName.c_str(), funcName.c_str());
        return;
    }

    uint8_t head[0xD0] = {};
    uint8_t numParms = 0;
    uint16_t parmsSize = 0;
    if (ReadMemory(fn, head, sizeof(head))) {
        numParms = head[0xC4];
        memcpy(&parmsSize, head + 0xC6, sizeof(parmsSize));
    }
    LogLine("调用实验：%s::%s numParms=%u parmsSize=%u inst=0x%llX", clsName.c_str(),
            funcName.c_str(), numParms, parmsSize,
            static_cast<unsigned long long>(inst));

    alignas(16) uint8_t frame[256] = {};
    if (g_original != nullptr) {
        g_original(reinterpret_cast<void*>(inst), reinterpret_cast<void*>(fn), frame);
    }
    LogLine("调用实验：%s 已返回", funcName.c_str());
}

// 沿父类链找属性偏移：AvatarPawn 这类字段常声明在基类上。
bool FindPropInChain(uintptr_t cls, const char* name, uint32_t& outOffset) {
    uintptr_t c = cls;
    for (int depth = 0; c != 0 && depth < 8; ++depth) {
        ue::PropInfo prop;
        if (ue::FindProperty(c, name, prop)) {
            outOffset = prop.offset;
            return true;
        }
        uintptr_t super = 0;
        if (!Read(c + ue::kUStructSuper, super)) {
            return false;
        }
        c = super;
    }
    return false;
}

// 在游戏线程上按 ProcessEvent 调一个函数；parms 会拷进对齐缓冲（不能传 nullptr）。
bool CallScript(uintptr_t obj, uintptr_t fn, const void* parms, size_t size) {
    if (g_original == nullptr || obj == 0 || fn == 0 || size > 256) {
        return false;
    }
    alignas(16) uint8_t frame[256] = {};
    if (parms != nullptr && size > 0) {
        memcpy(frame, parms, size);
    }
    g_original(reinterpret_cast<void*>(obj), reinterpret_cast<void*>(fn), frame);
    return true;
}

// 本地玩家控制器：GWorld → GameInstance → LocalPlayers[0] → PlayerController。
uintptr_t ResolvePlayerController() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    uintptr_t world = 0;
    uintptr_t gameInstance = 0;
    uintptr_t players = 0;
    uintptr_t localPlayer = 0;
    uintptr_t controller = 0;
    if (!Read(base + Cfg().gworldRva, world) || !IsUserPointer(world) ||
        !Read(world + ue::kWorldOwningGameInstance, gameInstance) || !IsUserPointer(gameInstance) ||
        !Read(gameInstance + ue::kGameInstanceLocalPlayers, players) || !IsUserPointer(players) ||
        !Read(players, localPlayer) || !IsUserPointer(localPlayer) ||
        !Read(localPlayer + ue::kLocalPlayerController, controller) || !IsUserPointer(controller)) {
        return 0;
    }
    return controller;
}

// 玩家自己的角色（AvatarPawn）。它定义在 A_PawnModeManager 上，H 播放期间被隐藏。
uintptr_t ResolveAvatarPawn(bool allowScan) {
    if (g_pawnModeClass == 0) {
        if (!allowScan || !ue::FindClassByName("A_PawnModeManager", g_pawnModeClass)) {
            return 0;
        }
    }
    if (g_avatarOffset == 0 && !FindPropInChain(g_pawnModeClass, "AvatarPawn", g_avatarOffset)) {
        return 0;
    }

    static uintptr_t manager = 0;
    if (manager != 0 && !ue::IsAlive(manager, g_pawnModeClass)) {
        manager = 0;
    }
    if (manager == 0) {
        if (!allowScan || !ue::FindInstanceOfClass(g_pawnModeClass, nullptr, manager)) {
            return 0;
        }
    }

    uintptr_t avatar = 0;
    if (!Read(manager + g_avatarOffset, avatar) || !IsUserPointer(avatar)) {
        return 0;
    }
    return avatar;
}

// SetActorHiddenInGame(false) —— 单独抽出来，兼顾"保持期"每帧补写。
bool ApplyAvatarVisible(uintptr_t avatar) {
    if (avatar == 0 || g_fnSetHidden == 0) {
        return false;
    }
    const uint8_t hidden = 0;
    return CallScript(avatar, g_fnSetHidden, &hidden, sizeof(hidden));
}

// 保持期：游戏可能在下一帧又把玩家藏起来（H 场景每帧驱动），所以 800ms 内每 16ms 补一次。
void TickRestoreHold() {
    if (InterlockedCompareExchange(&g_restoreActive, 0, 0) == 0) {
        return;
    }
    const DWORD now = ::GetTickCount();
    const LONG until = InterlockedCompareExchange(&g_restoreUntil, 0, 0);
    if (until == 0 || static_cast<LONG>(now - until) >= 0) {
        return;
    }
    const DWORD last = static_cast<DWORD>(InterlockedCompareExchange(&g_lastRestoreApply, 0, 0));
    if (now - last < 16) {
        return;
    }
    InterlockedExchange(&g_lastRestoreApply, static_cast<LONG>(now));
    ApplyAvatarVisible(ResolveAvatarPawn(false));
}

// 收集某个 actor 自己的输入组件（Outer == actor 且类名以 InputComponent 结尾）。
// A_ClientHController 同时挂了 HInputComp 和 HEnhancedInputComp，而 AActor::DisableInput
// 只会从输入栈里摘掉 Actor->InputComponent 那一个，所以必须逐个处理。
struct InputCompScan {
    uintptr_t outer;
    uintptr_t found[4];
    int count;
};

bool VisitInputComp(uintptr_t object, void* user) {
    auto* scan = static_cast<InputCompScan*>(user);
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!ue::ReadObjectHeader(object, cls, index, number, outer) || outer != scan->outer) {
        return true;
    }
    const std::string className = ue::ClassNameOf(object);
    static const char kSuffix[] = "InputComponent";
    const size_t suffixLen = sizeof(kSuffix) - 1;
    if (className.size() < suffixLen ||
        className.compare(className.size() - suffixLen, suffixLen, kSuffix) != 0) {
        return true;
    }
    if (scan->count < 4) {
        scan->found[scan->count++] = object;
    }
    return true;
}

// 逐个把 H 控制器的输入组件从玩家控制器的输入栈里推出（enable=false）或推回（enable=true）。
int SetHInputsEnabled(uintptr_t hController, uintptr_t playerController, bool enable) {
    if (hController == 0 || playerController == 0) {
        return 0;
    }
    if (g_actorClass == 0) {
        return 0;
    }
    if (g_inputCompOffset == 0) {
        ue::PropInfo prop;
        if (!ue::FindProperty(g_actorClass, "InputComponent", prop)) {
            LogLine("切换控制：Actor::InputComponent 偏移解析失败");
            return 0;
        }
        g_inputCompOffset = prop.offset;
    }
    if (g_hInputCompCount < 0) {
        g_hInputCompCount = 0;
        // 先取两个已知属性：HInputComp / HEnhancedInputComp（它们可能不是该 actor 的子对象，
        // 靠 Outer 扫不到，实测只扫到 1 个，漏掉的那组照样在吃 WASD）。
        const char* const kProps[] = {"HInputComp", "HEnhancedInputComp", "InputComponent"};
        uintptr_t hclass = 0;
        uint32_t idx = 0;
        uint32_t num = 0;
        uintptr_t outer = 0;
        if (ue::ReadObjectHeader(hController, hclass, idx, num, outer)) {
            for (const char* name : kProps) {
                uint32_t offset = 0;
                if (!FindPropInChain(hclass, name, offset)) {
                    continue;
                }
                uintptr_t comp = 0;
                if (!Read(hController + offset, comp) || !IsUserPointer(comp)) {
                    continue;
                }
                bool duplicated = false;
                for (int i = 0; i < g_hInputCompCount; ++i) {
                    if (g_hInputComps[i] == comp) {
                        duplicated = true;
                    }
                }
                if (!duplicated && g_hInputCompCount < 4) {
                    g_hInputComps[g_hInputCompCount++] = comp;
                }
            }
        }
        InputCompScan scan{hController, {}, 0};
        ue::ScanObjects(&VisitInputComp, &scan);
        for (int i = 0; i < scan.count && g_hInputCompCount < 4; ++i) {
            bool duplicated = false;
            for (int j = 0; j < g_hInputCompCount; ++j) {
                if (g_hInputComps[j] == scan.found[i]) {
                    duplicated = true;
                }
            }
            if (!duplicated) {
                g_hInputComps[g_hInputCompCount++] = scan.found[i];
            }
        }
        LogLine("切换控制：H 控制器输入组件 %d 个", g_hInputCompCount);
    }

    const uintptr_t fn = enable ? g_fnEnableInput : g_fnDisableInput;
    if (fn == 0) {
        return 0;
    }

    uintptr_t original = 0;
    Read(hController + g_inputCompOffset, original);
    int handled = 0;
    for (int i = 0; i < g_hInputCompCount; ++i) {
        if (!IsUserPointer(g_hInputComps[i])) {
            continue;
        }
        // AActor::DisableInput/EnableInput 只处理 Actor->InputComponent 这一个字段，
        // 所以逐个把它替换成目标组件再调用。
        Write(hController + g_inputCompOffset, g_hInputComps[i]);
        uintptr_t param = playerController;
        CallScript(hController, fn, &param, sizeof(param));
        ++handled;
    }
    Write(hController + g_inputCompOffset, original);
    return handled;
}

// 单个 actor 的输入开关（FreeCamPawn 这种自带输入组件的 actor 用）。
void SetActorInputEnabled(uintptr_t actor, uintptr_t playerController, bool enable) {
    if (!IsUserPointer(actor) || playerController == 0) {
        return;
    }
    const uintptr_t fn = enable ? g_fnEnableInput : g_fnDisableInput;
    if (fn == 0) {
        return;
    }
    uintptr_t param = playerController;
    CallScript(actor, fn, &param, sizeof(param));
}

// H 播放期间接管输入的那个控制器。
uintptr_t ResolveHController() {
    static uintptr_t cls = 0;
    static uintptr_t inst = 0;
    if (cls == 0 && !ue::FindClassByName("A_ClientHController", cls)) {
        return 0;
    }
    if (inst != 0 && !ue::IsAlive(inst, cls)) {
        inst = 0;
    }
    if (inst == 0 && !ue::FindInstanceOfClass(cls, nullptr, inst)) {
        return 0;
    }
    return inst;
}

// P 键主体：把控制权和镜头一起交给玩家角色，H 场景保持不动。
//
// 关键点（都是实测踩出来的）：
//   1) 只 possess 不够，H 期间角色自己的输入组件被摘掉了，要 EnableInput 推回去；
//   2) H 控制器上挂着两组输入组件，AActor::DisableInput 只摘 Actor->InputComponent 一个，
//      必须逐个挪到该字段上再摘，否则 WASD 仍会拖动镜头；
//   3) 切回时把两组输入推回去，否则 ESC 失灵、退不出播放。
// 是否已切换用"当前 pawn 是不是 AvatarPawn"判断，不维护自己的标志位（会和游戏状态失同步）。
void ExecToggleRestore() {
    const uintptr_t controller = ResolvePlayerController();
    if (controller == 0) {
        LogLine("切换控制：找不到本地玩家控制器");
        return;
    }
    const uintptr_t avatar = ResolveAvatarPawn(true);
    if (avatar == 0) {
        LogLine("切换控制：找不到 AvatarPawn");
        return;
    }

    if (g_actorClass == 0) {
        ue::FindClassByName("Actor", g_actorClass);
    }
    if (g_controllerClass == 0) {
        ue::FindClassByName("Controller", g_controllerClass);
    }
    if (g_actorClass != 0) {
        if (g_fnSetHidden == 0) {
            FindFunctionInChain(g_actorClass, "SetActorHiddenInGame", g_fnSetHidden);
        }
        if (g_fnEnableInput == 0) {
            FindFunctionInChain(g_actorClass, "EnableInput", g_fnEnableInput);
        }
        if (g_fnDisableInput == 0) {
            FindFunctionInChain(g_actorClass, "DisableInput", g_fnDisableInput);
        }
    }
    if (g_controllerClass != 0) {
        if (g_fnPossess == 0) {
            FindFunctionInChain(g_controllerClass, "Possess", g_fnPossess);
        }
        if (g_fnSetViewTarget == 0) {
            // SetViewTarget 不是 BlueprintCallable，反射里查不到；用 PlayerController 这一族。
            uintptr_t pcClass = 0;
            if (ue::FindClassByName("PlayerController", pcClass)) {
                FindFunctionInChain(pcClass, "SetViewTargetWithBlend", g_fnSetViewTarget);
                if (g_fnSetViewTarget == 0) {
                    FindFunctionInChain(pcClass, "ClientSetViewTarget", g_fnSetViewTarget);
                }
            }
        }
    }
    if (g_fnPossess == 0) {
        LogLine("切换控制：Possess 未解析");
        return;
    }

    // 不能用"pawn 是不是 AvatarPawn"判断：H 模式下玩家角色只是被隐藏，并没有被 unpossess，
    // pawn 一直是 AvatarPawn（实测）。所以用标志位，并让它在游戏的播放/停止事件里自动复位。
    const bool inPlayerMode = InterlockedCompareExchange(&g_restoreActive, 0, 0) != 0;
    const uintptr_t hController = ResolveHController();

    if (!inPlayerMode) {
        const bool okVisible = ApplyAvatarVisible(avatar);

        uintptr_t param = controller;
        const bool okEnable = g_fnEnableInput != 0 &&
                              CallScript(avatar, g_fnEnableInput, &param, sizeof(param));
        param = avatar;
        const bool okPossess = CallScript(controller, g_fnPossess, &param, sizeof(param));
        bool okView = false;
        if (g_fnSetViewTarget != 0) {
            uintptr_t view = avatar;
            okView = CallScript(controller, g_fnSetViewTarget, &view, sizeof(view));
        }
        // 自由镜头 pawn 才是真正在吃 WASD 的那个（实测：所有 Move* 事件都落在它身上），
        // 不关掉它，角色和镜头会同时动，表现就是"移动速度翻倍 + 镜头乱跑"。
        uintptr_t freeCam = 0;
        if (hController != 0) {
            if (g_freeCamOffset == 0) {
                uintptr_t hclass = 0;
                uint32_t idx = 0;
                uint32_t num = 0;
                uintptr_t outer = 0;
                if (ue::ReadObjectHeader(hController, hclass, idx, num, outer)) {
                    FindPropInChain(hclass, "FreeCam", g_freeCamOffset);
                }
            }
            if (g_freeCamOffset != 0) {
                Read(hController + g_freeCamOffset, freeCam);
            }
        }
        SetActorInputEnabled(freeCam, controller, false);

        const int popped = SetHInputsEnabled(hController, controller, false);

        uintptr_t pawnNow = 0;
        Read(controller + ue::kControllerPawn, pawnNow);

        g_probeCount = 0;
        InterlockedExchange(&g_restoreActive, 1);
        InterlockedExchange(&g_restoreUntil, static_cast<LONG>(::GetTickCount() + 1200));
        LogLine("切换控制 → 角色：avatar=0x%llX pawnNow=0x%llX 显示=%d 输入=%d 接管=%d 镜头=%d "
                "摘H输入=%d",
                static_cast<unsigned long long>(avatar),
                static_cast<unsigned long long>(pawnNow), okVisible ? 1 : 0, okEnable ? 1 : 0,
                okPossess ? 1 : 0, okView ? 1 : 0, popped);
        LogLine("切换控制：指针 hidden=0x%llX view=0x%llX inputOff=0x%X hc=0x%llX",
                static_cast<unsigned long long>(g_fnSetHidden),
                static_cast<unsigned long long>(g_fnSetViewTarget), g_inputCompOffset,
                static_cast<unsigned long long>(hController));
        return;
    }

    // 切回 H 模式：先把 H 的输入推回去，再把控制权交还自由镜头。
    const int pushed = SetHInputsEnabled(hController, controller, true);
    uintptr_t freeCam = 0;
    if (hController != 0) {
        if (g_freeCamOffset == 0) {
            uintptr_t hclass = 0;
            uint32_t idx = 0;
            uint32_t num = 0;
            uintptr_t outer = 0;
            if (ue::ReadObjectHeader(hController, hclass, idx, num, outer)) {
                FindPropInChain(hclass, "FreeCam", g_freeCamOffset);
            }
        }
        if (g_freeCamOffset != 0) {
            Read(hController + g_freeCamOffset, freeCam);
        }
    }
    if (IsUserPointer(freeCam)) {
        SetActorInputEnabled(freeCam, controller, true);
        uintptr_t param = freeCam;
        CallScript(controller, g_fnPossess, &param, sizeof(param));
        if (g_fnSetViewTarget != 0) {
            uintptr_t view = freeCam;
            CallScript(controller, g_fnSetViewTarget, &view, sizeof(view));
        }
    }
    InterlockedExchange(&g_restoreActive, 0);
    InterlockedExchange(&g_restoreUntil, 0);
    LogLine("切换控制 → H 模式：推回输入=%d freeCam=0x%llX", pushed,
            static_cast<unsigned long long>(freeCam));
}

// 把钩子累加的鼠标位移转成控制器视角（AddYawInput / AddPitchInput）。
// 为什么自己做：H 期间鼠标事件被 A_FreeCameraPawn 独占，停掉它之后没人再处理鼠标。
void ApplyPendingLook() {
    if (InterlockedCompareExchange(&g_restoreActive, 0, 0) == 0) {
        return;
    }
    const DWORD now = ::GetTickCount();
    if (now - static_cast<DWORD>(InterlockedCompareExchange(&g_lastLookApply, 0, 0)) < 8) {
        return;
    }
    const LONG dx = InterlockedExchange(&g_lookDx, 0);
    const LONG dy = InterlockedExchange(&g_lookDy, 0);
    if (dx == 0 && dy == 0) {
        return;
    }
    InterlockedExchange(&g_lastLookApply, static_cast<LONG>(now));

    if (g_fnAddYaw == 0 || g_fnAddPitch == 0) {
        uintptr_t pcClass = 0;
        if (!ue::FindClassByName("PlayerController", pcClass)) {
            return;
        }
        FindFunctionInChain(pcClass, "AddYawInput", g_fnAddYaw);
        FindFunctionInChain(pcClass, "AddPitchInput", g_fnAddPitch);
        LogLine("鼠标视角：AddYawInput=0x%llX AddPitchInput=0x%llX",
                static_cast<unsigned long long>(g_fnAddYaw),
                static_cast<unsigned long long>(g_fnAddPitch));
    }
    const uintptr_t controller = ResolvePlayerController();
    if (controller == 0 || g_fnAddYaw == 0) {
        return;
    }
    const float scale = static_cast<float>(Cfg().lookSensitivity);
    const float yaw = static_cast<float>(dx) * scale;
    const float pitch = static_cast<float>(dy) * scale * (Cfg().lookInvertY ? 1.0f : -1.0f);
    CallScript(controller, g_fnAddYaw, &yaw, sizeof(yaw));
    if (g_fnAddPitch != 0) {
        CallScript(controller, g_fnAddPitch, &pitch, sizeof(pitch));
    }
}

// 空格切镜头：普通模式下由 A_ChangeRoomManager 的输入事件处理，H 模式下它没被激活，
// 所以按空格没反应。这里由键盘钩子转发一次，行为与普通模式一致。
void ExecCameraSwitch() {
    static uintptr_t roomClass = 0;
    static uintptr_t roomFn = 0;
    if (roomClass == 0) {
        if (!ue::FindClassByName("A_ChangeRoomManager", roomClass)) {
            LogLine("切镜头：找不到 A_ChangeRoomManager");
            return;
        }
        roomFn = FindFunctionByPrefix(roomClass, "InpActEvt_SpaceBar");
        LogLine("切镜头：class=0x%llX fn=0x%llX",
                static_cast<unsigned long long>(roomClass),
                static_cast<unsigned long long>(roomFn));
    }
    if (roomFn == 0) {
        return;
    }
    uintptr_t instance = 0;
    if (!ue::FindInstanceOfClass(roomClass, nullptr, instance)) {
        LogLine("切镜头：A_ChangeRoomManager 没有实例");
        return;
    }
    CallScript(instance, roomFn, nullptr, 0);
    LogLine("切镜头：已转发空格到 A_ChangeRoomManager");
}

// 实验（O 键）：走游戏自己的退出流程，拦掉"停止播放 + 销毁"的反射调用。
// 目的是验证销毁到底走不走反射——如果走，这条路就能同时拿到原生镜头/UI 和持续播放。
void ExecQuitExperiment() {
    const uintptr_t hController = ResolveHController();
    if (hController == 0) {
        LogLine("实验退出：找不到 A_ClientHController");
        return;
    }

    // 是否在播放：MyHPerformer 非空。
    if (g_performerOffset == 0) {
        uintptr_t hclass = 0;
        uint32_t idx = 0;
        uint32_t num = 0;
        uintptr_t outer = 0;
        if (ue::ReadObjectHeader(hController, hclass, idx, num, outer)) {
            FindPropInChain(hclass, "MyHPerformer", g_performerOffset);
        }
    }
    uintptr_t performer = 0;
    if (g_performerOffset == 0 || !Read(hController + g_performerOffset, performer) ||
        !IsUserPointer(performer)) {
        LogLine("实验退出：当前不在播放中，忽略");
        return;
    }

    if (g_fnOnEscPressed == 0) {
        uintptr_t hclass = 0;
        uint32_t idx = 0;
        uint32_t num = 0;
        uintptr_t outer = 0;
        if (!ue::ReadObjectHeader(hController, hclass, idx, num, outer) ||
            !FindFunctionInChain(hclass, "OnEscPressed", g_fnOnEscPressed)) {
            LogLine("实验退出：找不到 OnEscPressed");
            return;
        }
    }

    InstallDestroyHook();
    InterlockedExchange(&g_suppressUntil, static_cast<LONG>(::GetTickCount() + 3000));
    LogLine("实验退出：触发退出流程，3 秒内拦下 %s（并阻止 Actor 销毁）",
            kStopTokens[0]);
    CallScript(hController, g_fnOnEscPressed, nullptr, 0);
    LogLine("实验退出：退出流程已返回");
}

void __cdecl ProcessEventThunk(void* self, void* function, void* parms) {
    // CLI 排队的强制提交：必须在游戏线程（也就是这里）执行，
    // 从注入器线程直接调 ProcessEvent 会和游戏线程死锁（已实测）。
    if (InterlockedExchange(&g_pendingCommit, 0) != 0) {
        ExecCommit("CLI");
    }
    if (InterlockedExchange(&g_pendingCall, 0) != 0) {
        ExecCall();
    }
    if (InterlockedExchange(&g_pendingRestore, 0) != 0) {
        ExecToggleRestore();
    }
    if (InterlockedExchange(&g_pendingQuitExp, 0) != 0) {
        ExecQuitExperiment();
    }
    if (InterlockedExchange(&g_pendingCamSwitch, 0) != 0) {
        ExecCameraSwitch();
    }
    TickRestoreHold();
    ApplyPendingLook();

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
    // 抑制窗口内：把"停止播放"的调用吞掉（只记录，不执行原逻辑）。
    // 这是"保持播放"能成立的关键——退出流程照常跑，唯独收尾那几步不生效。
    bool suppressed = false;
    if (Suppressing() && function != nullptr) {
        const std::string fnName = ue::ObjectName(reinterpret_cast<uintptr_t>(function));
        if (IsStopPlaybackCall(fnName)) {
            suppressed = true;
            const std::string owner =
                self != nullptr ? ue::ClassNameOf(reinterpret_cast<uintptr_t>(self)) : std::string();
            LogLine("保持播放：拦下 %s::%s", owner.c_str(), fnName.c_str());
        }
    }

    if (!suppressed && g_original != nullptr) {
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
        LogLine("脚本追踪：找不到 A_ClientHPreviewManager");
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
        LogLine("脚本追踪：没有管理器实例，无法取 vtable");
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
    // 先把 K2_DestroyActor 的 Func 还回去：这个指针指向本 DLL 的代码，
    // 卸载时不还原就是一个悬空指针，下一次销毁 Actor 会直接跳进已释放内存崩溃（已实测）。
    if (g_k2DestroySlot != nullptr && g_origK2Destroy != nullptr) {
        void* current = nullptr;
        if (Read(reinterpret_cast<uintptr_t>(g_k2DestroySlot), current) &&
            current == reinterpret_cast<void*>(&K2DestroyThunk)) {
            if (Write(reinterpret_cast<uintptr_t>(g_k2DestroySlot),
                      reinterpret_cast<void*>(g_origK2Destroy))) {
                LogLine("销毁钩子：已还原 K2_DestroyActor::Func");
            } else {
                LogLine("销毁钩子：还原 Func 失败（切勿卸载，可能崩溃）");
            }
        }
        g_k2DestroySlot = nullptr;
        g_origK2Destroy = nullptr;
    }

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

int ScriptTraceCallFromFile() {
    if (!g_installed || g_original == nullptr) {
        LogLine("调用实验：脚本追踪未接管，无法调用");
        return -1;
    }

    // call.txt 与 DLL 同目录，两行：类名 / 函数名。改文件即可换目标，不必重编译。
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(
                                  reinterpret_cast<uintptr_t>(&ScriptTraceCallFromFile)),
                              &mod)) {
        return -2;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD len = ::GetModuleFileNameW(mod, path, MAX_PATH);
    if (len == 0) {
        return -2;
    }
    std::wstring file(path, len);
    const size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return -2;
    }
    file.resize(slash + 1);
    file += L"call.txt";

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, file.c_str(), L"rb") != 0 || fp == nullptr) {
        LogLine("调用实验：DLL 目录下没有 call.txt");
        return -3;
    }
    std::string items[2];
    int got = 0;
    char line[256] = {};
    while (got < 2 && std::fgets(line, sizeof(line), fp) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == ';' || s[0] == '#') {
            continue;
        }
        items[got++] = s;
    }
    std::fclose(fp);
    if (got < 2) {
        LogLine("调用实验：call.txt 需要两行（类名、函数名）");
        return -4;
    }

    strncpy_s(g_callClass, items[0].c_str(), _TRUNCATE);
    strncpy_s(g_callFunc, items[1].c_str(), _TRUNCATE);
    InterlockedExchange(&g_pendingCall, 1);
    LogLine("调用实验：已排队 %s::%s", items[0].c_str(), items[1].c_str());
    return 0;
}

void ScriptTraceRequestRestore() {
    if (!g_installed) {
        LogLine("恢复玩家：脚本追踪未接管，忽略");
        return;
    }
    InterlockedExchange(&g_pendingRestore, 1);
}

bool ScriptTraceInPlayerMode() {
    return InterlockedCompareExchange(&g_restoreActive, 0, 0) != 0;
}

void ScriptTraceRequestCameraSwitch() {
    if (!g_installed) {
        return;
    }
    InterlockedExchange(&g_pendingCamSwitch, 1);
}

void ScriptTraceAddLook(long dx, long dy) {
    if (InterlockedCompareExchange(&g_restoreActive, 0, 0) == 0) {
        return;
    }
    InterlockedExchangeAdd(&g_lookDx, static_cast<LONG>(dx));
    InterlockedExchangeAdd(&g_lookDy, static_cast<LONG>(dy));
}

void ScriptTraceRequestQuitExperiment() {
    if (!g_installed) {
        return;
    }
    InterlockedExchange(&g_pendingQuitExp, 1);
}

}  // namespace ge
