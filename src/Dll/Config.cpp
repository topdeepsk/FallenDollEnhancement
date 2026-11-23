#include "Dll/Config.h"

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"

namespace ge {

namespace {

Config g_config;
std::wstring g_iniPath;

std::wstring Trim(const std::wstring& text) {
    const wchar_t* ws = L" \t\r\n";
    const size_t first = text.find_first_not_of(ws);
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return text;
}

std::wstring MakeIniPath(HMODULE module) {    const std::wstring dllPath = GetModuleFilePath(module);
    if (dllPath.empty()) {
        return L"GameEnhancement.ini";
    }
    const size_t slash = dllPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"GameEnhancement.ini";
    }
    return dllPath.substr(0, slash + 1) + L"GameEnhancement.ini";
}

void WriteDefaultIni(const Config& config) {
    std::ofstream out(g_iniPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    char hex[32] = {};
    _snprintf_s(hex, sizeof(hex), _TRUNCATE, "0x%08llX",
                static_cast<unsigned long long>(config.gworldRva));
    char wz[32] = {};
    _snprintf_s(wz, sizeof(wz), _TRUNCATE, "0x%03llX",
                static_cast<unsigned long long>(config.worldZOffset));

    out << "; GameEnhancement —— 修改后重新注入即可生效\n"
        << "; 游戏更新后 GWorldRVA 失效，重新 dump 后改这里\n"
        << "GWorldRVA=" << hex << "\n"
        << "; RootComponent 里 ComponentToWorld.Translation.Z 的偏移（非反射字段，需手工定位）\n"
        << "; 只写 RelativeLocation 不会触发 UpdateComponentToWorld，画面不会动，所以两个都要写\n"
        << "WorldZOffset=" << wz << "\n"
        << "JumpHeight=" << config.jumpHeight << "\n"
        << "HoldMs=" << config.holdMs << "\n"
        << "TickMs=" << config.tickMs << "\n"
        << "HotkeyVK=0x" << std::hex << config.hotkeyVk << std::dec << "\n"
        << "ZeroVelocity=" << (config.zeroVelocity ? 1 : 0) << "\n"
        << "\n"
        << "; ---- 动作放置（红色区域也能落位）----\n"
        << "; 左键确认时，把预览管理器上的判定状态改写成\"已吸附\"，让游戏走自己的提交路径。\n"
        << "; 只在游戏自己判定为无效（item/wall/floor 三个都为假）时介入，不干扰正常放置。\n"
        << "PreviewFix=" << (config.previewFix ? 1 : 0) << "\n"
        << "PreviewLogOnly=" << (config.previewLogOnly ? 1 : 0) << "\n"
        << "PreviewOnlyUnsnapped=" << (config.previewOnlyUnsnapped ? 1 : 0) << "\n"
        << "PreviewSetSnapWall=" << (config.previewSetSnapWall ? 1 : 0) << "\n"
        << "PreviewSetFloor=" << (config.previewSetFloor ? 1 : 0) << "\n"
        << "PreviewSetZ=" << (config.previewSetZ ? 1 : 0) << "\n"
        << "PreviewWriteHitLoc=" << (config.previewWriteHitLoc ? 1 : 0) << "\n"
        << "PreviewHoldMs=" << config.previewHoldMs << "\n"
        << "\n"
        << "; ---- 脚本调用追踪 ----\n"
        << "; 接管 A_ClientHPreviewManager 的 ProcessEvent（只记录调用，不改变行为）。\n"
        << "TraceScript=" << (config.traceScript ? 1 : 0) << "\n"
        << "; UE5.7 的 UObject::ProcessEvent 在 vtable 的偏移；失配时会自动扫描特征\n"
        << "ProcessEventSlot=0x" << std::hex << config.processEventSlot << std::dec << "\n"
        << "; 1 = 只定位槽位、不改 vtable（第一次验证用）\n"
        << "TraceDryRun=" << (config.traceDryRun ? 1 : 0) << "\n"
        << "; 1 = 左键判定未通过时自动补调 OnHCheckPlayHCard / OnHPrePlayHCard\n"
        << "PreviewAutoCommit=" << (config.previewAutoCommit ? 1 : 0) << "\n";
}

bool ParseBoolValue(const std::wstring& value, bool& out) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 0);
    if (end == value.c_str()) {
        return false;
    }
    out = parsed != 0;
    return true;
}

void ApplyLine(const std::wstring& rawLine, Config& config) {
    const std::wstring line = Trim(rawLine);
    if (line.empty() || line[0] == L';' || line[0] == L'#') {
        return;
    }
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) {
        return;
    }

    const std::wstring key = ToLower(Trim(line.substr(0, eq)));
    const std::wstring value = Trim(line.substr(eq + 1));
    if (value.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    if (key == L"gworldrva") {
        const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed != 0) {
            config.gworldRva = static_cast<uintptr_t>(parsed);
        }
    } else if (key == L"worldzoffset") {
        const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 0x100 && parsed < 0x1000) {
            config.worldZOffset = static_cast<uintptr_t>(parsed);
        }
    } else if (key == L"jumpheight") {
        const double parsed = std::wcstod(value.c_str(), &end);
        if (end != value.c_str() && parsed > 0.0 && parsed < 100000.0) {
            config.jumpHeight = parsed;
        }
    } else if (key == L"holdms") {
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 0 && parsed <= 5000) {
            config.holdMs = static_cast<int>(parsed);
        }
    } else if (key == L"tickms") {
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 1 && parsed <= 100) {
            config.tickMs = static_cast<int>(parsed);
        }
    } else if (key == L"hotkeyvk") {
        const unsigned long parsed = std::wcstoul(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 1 && parsed <= 0xFF) {
            config.hotkeyVk = static_cast<UINT>(parsed);
        }
    } else if (key == L"zerovelocity") {
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        if (end != value.c_str()) {
            config.zeroVelocity = parsed != 0;
        }
    } else if (key == L"previewfix") {
        ParseBoolValue(value, config.previewFix);
    } else if (key == L"previewlogonly") {
        ParseBoolValue(value, config.previewLogOnly);
    } else if (key == L"previewonlyunsnapped") {
        ParseBoolValue(value, config.previewOnlyUnsnapped);
    } else if (key == L"previewsetsnapwall") {
        ParseBoolValue(value, config.previewSetSnapWall);
    } else if (key == L"previewsetfloor") {
        ParseBoolValue(value, config.previewSetFloor);
    } else if (key == L"previewsetz") {
        ParseBoolValue(value, config.previewSetZ);
    } else if (key == L"previewwritehitloc") {
        ParseBoolValue(value, config.previewWriteHitLoc);
    } else if (key == L"previewholdms") {
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 0 && parsed <= 2000) {
            config.previewHoldMs = static_cast<int>(parsed);
        }
    } else if (key == L"tracescript") {
        ParseBoolValue(value, config.traceScript);
    } else if (key == L"tracedryrun") {
        ParseBoolValue(value, config.traceDryRun);
    } else if (key == L"looksensitivity") {
        const double parsed = std::wcstod(value.c_str(), &end);
        if (end != value.c_str() && parsed > 0.0 && parsed <= 5.0) {
            config.lookSensitivity = parsed;
        }
    } else if (key == L"lookinverty") {
        ParseBoolValue(value, config.lookInvertY);
    } else if (key == L"probeinput") {
        ParseBoolValue(value, config.probeInput);
    } else if (key == L"previewautocommit") {
        ParseBoolValue(value, config.previewAutoCommit);
    } else if (key == L"processeventslot") {
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        if (end != value.c_str() && parsed >= 0 && parsed <= 0x800) {
            config.processEventSlot = static_cast<uint32_t>(parsed);
        }
    }
}

}  // namespace

Config& Cfg() {
    return g_config;
}

void LoadConfig(HMODULE module) {
    g_config = Config{};
    g_config.gworldRva = ue::kGWorldRva;
    g_config.worldZOffset = ue::kRootWorldZ;

    g_iniPath = MakeIniPath(module);

    std::ifstream in(g_iniPath, std::ios::binary);
    if (!in) {
        WriteDefaultIni(g_config);
        LogLine("配置：未找到 ini，已写出默认值 -> %s", Narrow(g_iniPath).c_str());
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // ini 里只可能出现 ASCII 键值，逐字节转宽字符足够。
        const std::wstring wide(line.begin(), line.end());
        ApplyLine(wide, g_config);
    }

    if (g_config.gworldRva == 0) {
        g_config.gworldRva = ue::kGWorldRva;
    }
    if (g_config.worldZOffset == 0) {
        g_config.worldZOffset = ue::kRootWorldZ;
    }

    LogLine("配置：rva=0x%08llX height=%.1f hold=%dms tick=%dms vk=0x%02X",
            static_cast<unsigned long long>(g_config.gworldRva), g_config.jumpHeight,
            g_config.holdMs, g_config.tickMs, g_config.hotkeyVk);
    LogLine("配置：放置修复 fix=%d logOnly=%d onlyUnsnapped=%d wall=%d floor=%d z=%d hit=%d hold=%dms",
            g_config.previewFix ? 1 : 0, g_config.previewLogOnly ? 1 : 0,
            g_config.previewOnlyUnsnapped ? 1 : 0, g_config.previewSetSnapWall ? 1 : 0,
            g_config.previewSetFloor ? 1 : 0, g_config.previewSetZ ? 1 : 0,
            g_config.previewWriteHitLoc ? 1 : 0, g_config.previewHoldMs);
    LogLine("配置：脚本追踪 trace=%d slot=0x%X", g_config.traceScript ? 1 : 0,
            g_config.processEventSlot);
}

}  // namespace ge
