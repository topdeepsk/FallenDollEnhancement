import ctypes, ctypes.wintypes as wt

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
BASE_EXE = 0x7FF751640000
R_GOBJECTS = 0x0AB0D3D0
R_GNAMES   = 0x0AD5C080

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]

h = k32.OpenProcess(0x1F0FFF, False, 8504)
def rd(a, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw
def u64(b): return int.from_bytes(b, "little")

def vq(a):
    m = MBI()
    k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m))
    print("  vq 0x%X: base=0x%X alloc=0x%X size=0x%X state=%d prot=0x%X type=%d" % (
        a, m.BaseAddress or 0, m.AllocationBase or 0, m.RegionSize, m.State, m.Protect, m.Type))
    return m

# object 0 of chunk 0
gv = u64(rd(BASE_EXE + R_GOBJECTS, 8))
chunk0 = u64(rd(gv, 8))
item0 = rd(chunk0, 24)
obj = u64(item0[8:16])
print("item0 obj = 0x%X" % obj)
vq(obj)
vq(0x7FF419860000)

buf = rd(obj, 0x40)
print("obj bytes:", buf.hex())
cls = u64(buf[0x10:0x18]); nm = buf[0x18:0x20]; outer = u64(buf[0x20:0x28])
print("class=0x%X name_idx=%d number=%d outer=0x%X" % (cls, int.from_bytes(nm[0:4],'little'), int.from_bytes(nm[4:8],'little'), outer))

gn = BASE_EXE + R_GNAMES
def read_name(idx):
    blk = idx >> 16; off = (idx & 0xFFFF) * 2
    bp = u64(rd(gn + 0x10 + blk*8, 8))
    hdr_raw = rd(bp + off, 4)
    hdr = int.from_bytes(hdr_raw[0:2], "little")
    print("   hdr raw:", hdr_raw.hex(), "-> 0x%04X" % hdr)
    # UE5.4+: len = hdr & 0x3FF ; wide = bit 10
    ln = hdr & 0x3FF
    wide = (hdr >> 10) & 1
    print("   len", ln, "wide", wide)
    if wide:
        return rd(bp+off+2, ln*2).decode('utf-16-le','replace')
    return rd(bp+off+2, ln).decode('latin-1','replace')

print("name of obj:", repr(read_name(int.from_bytes(nm[0:4],'little'))))
