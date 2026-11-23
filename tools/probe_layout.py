import ctypes, ctypes.wintypes as wt, sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROC = 0x1F0FFF
BASE = 0x7FF751640000
R_GOBJECTS = 0x0AB0D3D0
R_GNAMES   = 0x0AD5C080

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]

h = k32.OpenProcess(PROC, False, 8504)
def rd(a, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw
def u64(b): return int.from_bytes(b, "little")
def i32(b): return int.from_bytes(b, "little", signed=True)
def readable(a):
    m = MBI()
    r = k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m))
    return r == ctypes.sizeof(m) and m.State == 0x1000

gv = u64(rd(BASE + R_GOBJECTS, 8))
print("GObjects var -> 0x%X" % gv)
for off in range(0, 0x40, 8):
    b = rd(gv + off, 8)
    print("  +0x%02X: %s  (int32 %d, %d)" % (off, b.hex(), i32(b[0:4]), i32(b[4:8])))
gn = BASE + R_GNAMES
print("GNames head:")
b = rd(gn, 0x40)
for off in range(0, 0x40, 8):
    print("  +0x%02X: %s" % (off, b[off:off+8].hex()))
