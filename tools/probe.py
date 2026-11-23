"""只读地验证玩家坐标解析链，不注入、不写内存。

用途：游戏更新后，先跑这个确认偏移是否还有效，再决定要不要改
GameEnhancement.ini / src/Common/UeOffsets.h。

用法：
    python tools/probe.py Paralogue-Win64-Shipping.exe
    python tools/probe.py Paralogue-Win64-Shipping.exe --bump 20

--bump 会做一次可逆的写测试：抬高 N 厘米、150ms 后读回、再还原原值，
用来确认"写 Z 是否真的推动角色"以及会不会被 CharacterMovement 立刻拉回。
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
LIST_MODULES_ALL = 0x03

GWORLD_RVA = 0x0AFB9F48
OFF_WORLD_GAME_INSTANCE = 0x228
OFF_GI_LOCAL_PLAYERS = 0x038
OFF_PLAYER_CONTROLLER = 0x030
OFF_CONTROLLER_PAWN = 0x2F0
OFF_ACTOR_ROOT = 0x1B8
OFF_SCENE_REL_LOC = 0x148
OFF_ROOT_WORLD_LOC = 0x210  # USceneComponent::ComponentToWorld.Translation（非反射字段）
OFF_ROOT_WORLD_Z = 0x220    # 上者 + 16


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", wt.WCHAR * 260),
    ]


def find_pid(name: str) -> int:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == -1:
        raise OSError("CreateToolhelp32Snapshot failed")
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pid = 0
    try:
        if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                if entry.szExeFile.lower() == name.lower():
                    pid = entry.th32ProcessID
                    break
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
    finally:
        kernel32.CloseHandle(snapshot)
    return pid


def module_base(pid: int) -> int:
    process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not process:
        raise OSError(ctypes.get_last_error(), "OpenProcess failed (需要同权限运行)")
    try:
        needed = wt.DWORD()
        psapi.EnumProcessModulesEx(process, None, 0, ctypes.byref(needed), LIST_MODULES_ALL)
        count = needed.value // ctypes.sizeof(ctypes.c_void_p)
        modules = (ctypes.c_void_p * count)()
        if not psapi.EnumProcessModulesEx(process, modules, needed.value,
                                          ctypes.byref(needed), LIST_MODULES_ALL):
            raise OSError("EnumProcessModulesEx failed")
        return int(modules[0])
    finally:
        kernel32.CloseHandle(process)


def read(process, address: int, size: int) -> bytes:
    buffer = ctypes.create_string_buffer(size)
    read_bytes = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(process, ctypes.c_void_p(address), buffer, size,
                                    ctypes.byref(read_bytes))
    if not ok or read_bytes.value != size:
        raise OSError(f"读 0x{address:X} 失败")
    return buffer.raw


def read_ptr(process, address: int) -> int:
    return int.from_bytes(read(process, address, 8), "little")


def write(process, address: int, data: bytes) -> bool:
    old = wt.DWORD(0)
    kernel32.VirtualProtectEx(process, ctypes.c_void_p(address), len(data),
                              0x04, ctypes.byref(old))  # PAGE_READWRITE
    written = ctypes.c_size_t(0)
    ok = kernel32.WriteProcessMemory(process, ctypes.c_void_p(address), data, len(data),
                                     ctypes.byref(written))
    kernel32.VirtualProtectEx(process, ctypes.c_void_p(address), len(data), old.value,
                              ctypes.byref(old))
    return bool(ok) and written.value == len(data)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    name = sys.argv[1]
    bump_cm = 0.0
    if "--bump" in sys.argv:
        index = sys.argv.index("--bump")
        if index + 1 < len(sys.argv):
            bump_cm = float(sys.argv[index + 1])

    pid = find_pid(name)
    if pid == 0:
        print(f"找不到进程 {name}")
        return 1

    base = module_base(pid)
    print(f"进程 {name}  PID={pid}  模块基址=0x{base:X}")

    access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if bump_cm != 0.0:
        # 只做读测试时不要写权限；--bump 才申请
        access |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION

    process = kernel32.OpenProcess(access, False, pid)
    if not process:
        print(f"OpenProcess 失败（错误码 {ctypes.get_last_error()}），请用管理员权限运行")
        return 1

    try:
        world = read_ptr(process, base + GWORLD_RVA)
        print(f"  GWorld          = 0x{world:X}")
        if world == 0:
            print("  -> GWorld 为空：还在加载，或 RVA 已过期")
            return 1

        game_instance = read_ptr(process, world + OFF_WORLD_GAME_INSTANCE)
        print(f"  GameInstance    = 0x{game_instance:X}")

        players_ptr = read_ptr(process, game_instance + OFF_GI_LOCAL_PLAYERS)
        count = int.from_bytes(read(process, game_instance + OFF_GI_LOCAL_PLAYERS + 8, 4), "little")
        print(f"  LocalPlayers    = 0x{players_ptr:X}  (Num={count})")
        if count <= 0:
            print("  -> 没有本地玩家（可能还在主菜单）")
            return 1

        local_player = read_ptr(process, players_ptr)
        print(f"  LocalPlayer[0]  = 0x{local_player:X}")

        controller = read_ptr(process, local_player + OFF_PLAYER_CONTROLLER)
        print(f"  PlayerController= 0x{controller:X}")

        pawn = read_ptr(process, controller + OFF_CONTROLLER_PAWN)
        print(f"  Pawn            = 0x{pawn:X}")
        if pawn == 0:
            print("  -> 未持有 Pawn")
            return 1

        root = read_ptr(process, pawn + OFF_ACTOR_ROOT)
        print(f"  RootComponent   = 0x{root:X}")

        x, y, z = (ctypes.c_double * 3).from_buffer_copy(
            read(process, root + OFF_SCENE_REL_LOC, 24))
        wx, wy, wz = (ctypes.c_double * 3).from_buffer_copy(
            read(process, root + OFF_ROOT_WORLD_LOC, 24))
        print(f"  RelativeLocation = ({x:.1f}, {y:.1f}, {z:.1f})")
        print(f"  世界坐标         = ({wx:.1f}, {wy:.1f}, {wz:.1f})   <- 权威值，DLL 写这个")
        if abs(wz - z) > 1.0:
            print(f"  注意：两者差 {z - wz:+.1f}，说明有历史写入残留在 RelativeLocation 里，属正常")

        if bump_cm != 0.0:
            rel_addr = root + OFF_SCENE_REL_LOC + 16
            world_addr = root + OFF_ROOT_WORLD_Z
            world_z = (ctypes.c_double * 1).from_buffer_copy(read(process, world_addr, 8))[0]
            original_world = ctypes.c_double(world_z)
            original_rel = ctypes.c_double(z)
            target = ctypes.c_double(world_z + bump_cm)

            # 必须两个都写：只改 RelativeLocation 不会触发 UpdateComponentToWorld，
            # 渲染与物理继续用缓存的世界变换，画面不会有任何变化。
            if not write(process, world_addr, bytes(target)):
                print("  -> 写世界变换失败")
                return 1
            write(process, rel_addr, bytes(target))

            time.sleep(0.15)
            readback = (ctypes.c_double * 1).from_buffer_copy(read(process, world_addr, 8))[0]
            write(process, world_addr, bytes(original_world))
            write(process, rel_addr, bytes(original_rel))  # 还原
            print(f"\n  写测试：world Z {world_z:.1f} -> {target.value:.1f}，150ms 后读回 "
                  f"{readback:.1f}（漂移 {readback - world_z:+.1f}），已还原")
            if abs(readback - world_z) < 1.0:
                print("  -> 世界变换没被接受：WorldZOffset 可能已过期")
            else:
                print("  -> 写入生效，角色确实被抬起来了")

        print("\n链路成立：DLL 会在这里写 Z + JumpHeight。")
        return 0
    except OSError as error:
        print(f"  -> {error}")
        print("  可能原因：GWorldRVA/字段偏移已随游戏更新失效，或当前不在可操控场景。")
        return 1
    finally:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    sys.exit(main())
