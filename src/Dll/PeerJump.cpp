#include "Dll/PeerJump.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"

namespace ge {

namespace {

// 角色自身的网络角色：0=None 1=SimulatedProxy 2=AutonomousProxy 3=Authority
constexpr uint32_t kActorRole = 0x168;
constexpr uint8_t kRoleAuthority = 3;

constexpr uint32_t kActorPawnClassSearch = 0;
constexpr size_t kMaxPawns = 16;

HANDLE g_peer = nullptr;
DWORD g_peerPid = 0;
uintptr_t g_peerBase = 0;
uintptr_t g_peerNames = 0;
uintptr_t g_peerPawns[kMaxPawns] = {};
int g_peerPawnCount = 0;
DWORD g_scanTick = 0;

bool R(uintptr_t address, void* buffer, size_t size) {
    if (g_peer == nullptr) {
        return false;
    }
    SIZE_T got = 0;
    return ::ReadProcessMemory(g_peer, reinterpret_cast<LPCVOID>(address), buffer, size, &got) ==
               TRUE &&
           got == size;
}

template <typename T>
bool ReadAt(uintptr_t address, T& out) {
    return R(address, &out, sizeof(T));
}

bool W(uintptr_t address, const void* buffer, size_t size) {
    if (g_peer == nullptr) {
        return false;
    }
    DWORD oldProtect = 0;
    if (!::VirtualProtectEx(g_peer, reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE,
                            &oldProtect)) {
        return false;
    }
    SIZE_T put = 0;
    const bool ok =
        ::WriteProcessMemory(g_peer, reinterpret_cast<LPVOID>(address), buffer, size, &put) == TRUE &&
        put == size;
    DWORD ignored = 0;
    ::VirtualProtectEx(g_peer, reinterpret_cast<LPVOID>(address), size, oldProtect, &ignored);
    return ok;
}

// 与 UeRuntime::ReadNameEntry 同一套编码，只是读取走远程句柄。
std::string PeerName(uint32_t index) {
    std::string out;
    if (g_peerNames == 0) {
        return out;
    }
    const uint32_t block = index >> 16;
    const uint32_t offset = (index & 0xFFFFu) * 2u;

    uintptr_t blockPtr = 0;
    if (!ReadAt(g_peerNames + ue::kNamePoolBlocks + static_cast<uintptr_t>(block) * 8, blockPtr) ||
        blockPtr == 0) {
        return out;
    }
    uint16_t header = 0;
    if (!ReadAt(blockPtr + offset, header)) {
        return out;
    }
    const uint32_t length = (header >> ue::kNameEntryLenShift) & 0x1FFu;
    const bool wide = (header & (1u << ue::kNameEntryWideBit)) != 0;
    if (length == 0 || length > ue::kNameEntryMaxLen) {
        return out;
    }
    if (!wide) {
        out.resize(length);
        if (!R(blockPtr + offset + 2, out.data(), length)) {
            out.clear();
        }
        return out;
    }
    std::wstring buffer(length, L'\0');
    if (!R(blockPtr + offset + 2, buffer.data(), length * sizeof(wchar_t))) {
        return out;
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), static_cast<int>(length),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return out;
    }
    out.resize(static_cast<size_t>(bytes));
    ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), static_cast<int>(length), out.data(), bytes,
                          nullptr, nullptr);
    return out;
}

bool OpenPeer() {
    if (g_peer != nullptr) {
        return true;
    }

    wchar_t selfPath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring selfName(selfPath);
    const size_t slash = selfName.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return false;
    }
    selfName.erase(0, slash + 1);

    DWORD pids[1024] = {};
    DWORD needed = 0;
    if (::EnumProcesses(pids, sizeof(pids), &needed) == FALSE) {
        return false;
    }

    const DWORD selfPid = ::GetCurrentProcessId();
    const size_t count = needed / sizeof(DWORD);
    for (size_t i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0 || pid == selfPid) {
            continue;
        }
        HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                           PROCESS_VM_OPERATION,
                                       FALSE, pid);
        if (process == nullptr) {
            continue;
        }
        wchar_t path[MAX_PATH] = {};
        DWORD len = static_cast<DWORD>(MAX_PATH);
        if (::QueryFullProcessImageNameW(process, 0, path, &len) == FALSE) {
            ::CloseHandle(process);
            continue;
        }
        std::wstring name(path, len);
        const size_t cut = name.find_last_of(L"\\/");
        if (cut != std::wstring::npos) {
            name.erase(0, cut + 1);
        }
        if (_wcsicmp(name.c_str(), selfName.c_str()) != 0) {
            ::CloseHandle(process);
            continue;
        }

        HMODULE modules[1] = {};
        DWORD neededBytes = 0;
        if (::EnumProcessModulesEx(process, modules, sizeof(modules), &neededBytes,
                                   LIST_MODULES_64BIT) == FALSE ||
            neededBytes == 0) {
            ::CloseHandle(process);
            continue;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(modules[0]);
        uintptr_t table = 0;
        SIZE_T got = 0;
        if (::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + ue::kGObjectsRva), &table,
                                sizeof(table), &got) == FALSE ||
            table == 0) {
            ::CloseHandle(process);
            continue;
        }
        g_peer = process;
        g_peerPid = pid;
        g_peerBase = base;
        // 名字池结构体就位于 base+kGNamesRva，不需要再解一次指针（解了就成随机值，
        // 后果是所有类名都读不出来，扫描结果恒为 0）。
        g_peerNames = base + ue::kGNamesRva;
        LogLine("跨进程跳跃：对端进程 PID=%lu base=0x%llX GNames=0x%llX", pid,
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(g_peerNames));
        return true;
    }
    return false;
}

// 遍历对端的 GUObjectArray，收集所有 AvatarPawn_C 实例。
int ScanPeerPawns() {
    g_peerPawnCount = 0;
    if (g_peer == nullptr) {
        return 0;
    }
    uintptr_t table = 0;
    if (!ReadAt(g_peerBase + ue::kGObjectsRva, table) || table == 0) {
        return 0;
    }

    const size_t stride = ue::kObjectItemStride;
    const size_t chunkBytes = static_cast<size_t>(ue::kObjectChunkSize) * stride;
    std::vector<uint8_t> buffer(chunkBytes);
    std::map<uintptr_t, std::string> classNameCache;

    for (uint32_t chunkIndex = 0; chunkIndex < 32; ++chunkIndex) {
        uintptr_t chunk = 0;
        if (!ReadAt(table + static_cast<uintptr_t>(chunkIndex) * 8, chunk) || chunk == 0) {
            break;
        }
        size_t done = 0;
        while (done < chunkBytes) {
            const size_t piece = std::min<size_t>(0x80000, chunkBytes - done);
            if (!R(chunk + done, buffer.data() + done, piece)) {
                break;
            }
            done += piece;
        }
        if (done < stride) {
            continue;
        }
        const uint32_t items = static_cast<uint32_t>(done / stride);
        for (uint32_t i = 0; i < items; ++i) {
            uintptr_t object = 0;
            memcpy(&object, buffer.data() + static_cast<size_t>(i) * stride + ue::kObjectItemObject,
                   sizeof(object));
            if (object == 0) {
                continue;
            }
            uintptr_t cls = 0;
            if (!ReadAt(object + ue::kUObjectClass, cls) || cls == 0) {
                continue;
            }
            auto it = classNameCache.find(cls);
            if (it == classNameCache.end()) {
                uint32_t nameIndex = 0;
                std::string name;
                if (ReadAt(cls + ue::kUObjectName, nameIndex)) {
                    name = PeerName(nameIndex);
                }
                it = classNameCache.emplace(cls, name).first;
            }
            if (it->second != "AvatarPawn_C") {
                continue;
            }
            if (g_peerPawnCount < static_cast<int>(kMaxPawns)) {
                g_peerPawns[g_peerPawnCount++] = object;
            }
        }
    }
    g_scanTick = ::GetTickCount();
    LogLine("跨进程跳跃：对端找到 %d 个 AvatarPawn_C", g_peerPawnCount);
    return g_peerPawnCount;
}

bool RoleIsAuthority(uintptr_t pawn) {
    uint8_t role = 0;
    return ReadAt(pawn + kActorRole, role) && role == kRoleAuthority;
}

}  // namespace

bool PeerJumpUp(const double localWorld[3], double height) {
    if (!OpenPeer()) {
        LogLine("跨进程跳跃：找不到同名的服务端进程（可能是远程服务器）");
        return false;
    }

    const DWORD now = ::GetTickCount();
    const bool cacheStale = g_peerPawnCount == 0 || (now - g_scanTick) > 5000;
    if (cacheStale && ScanPeerPawns() == 0) {
        return false;
    }

    // 按世界坐标最近邻匹配"我们的角色"：服务端和客户端的同一个 pawn 位置一致
    // （允许一点误差，网络同步有几十毫秒延迟）。
    uintptr_t best = 0;
    double bestDist = 0.0;
    for (int i = 0; i < g_peerPawnCount; ++i) {
        const uintptr_t pawn = g_peerPawns[i];
        if (!RoleIsAuthority(pawn)) {
            continue;
        }
        uintptr_t root = 0;
        if (!ReadAt(pawn + ue::kActorRootComponent, root) || root == 0) {
            continue;
        }
        double pos[3] = {};
        if (!R(root + ue::kRootWorldLocation, pos, sizeof(pos))) {
            continue;
        }
        const double dx = pos[0] - localWorld[0];
        const double dy = pos[1] - localWorld[1];
        const double dz = pos[2] - localWorld[2];
        const double dist = dx * dx + dy * dy + dz * dz;
        if (best == 0 || dist < bestDist) {
            best = pawn;
            bestDist = dist;
        }
    }
    if (best == 0) {
        LogLine("跨进程跳跃：对端没有 Authority 角色");
        return false;
    }
    if (bestDist > 400.0 * 400.0) {
        LogLine("跨进程跳跃：最近的 Authority 角色距离 %.0f，超出容忍范围，放弃",
                std::sqrt(bestDist));
        return false;
    }

    uintptr_t root = 0;
    uintptr_t movement = 0;
    if (!ReadAt(best + ue::kActorRootComponent, root) || root == 0 ||
        !ReadAt(best + ue::kCharacterMovement, movement) || movement == 0) {
        LogLine("跨进程跳跃：角色组件解析失败");
        return false;
    }

    double pos[3] = {};
    if (!R(root + ue::kRootWorldLocation, pos, sizeof(pos))) {
        LogLine("跨进程跳跃：读不到世界坐标");
        return false;
    }
    const double targetZ = pos[2] + height;
    if (!W(root + ue::kRootWorldLocation + 16, &targetZ, sizeof(targetZ))) {
        LogLine("跨进程跳跃：写世界 Z 失败");
        return false;
    }
    // 切成 Falling，服务端自己就会按重力下落；客户端随网络同步跟随。
    const uint8_t falling = ue::kMoveModeFalling;
    W(movement + ue::kCmcMovementMode, &falling, sizeof(falling));

    LogLine("跨进程跳跃：服务端角色 0x%llX Z %.1f -> %.1f（dist %.1f）",
            static_cast<unsigned long long>(best), pos[2], targetZ, std::sqrt(bestDist));
    return true;
}

void PeerJumpShutdown() {
    if (g_peer != nullptr) {
        ::CloseHandle(g_peer);
        g_peer = nullptr;
    }
    g_peerPid = 0;
    g_peerBase = 0;
    g_peerNames = 0;
    g_peerPawnCount = 0;
    g_scanTick = 0;
}

}  // namespace ge
