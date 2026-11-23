import ctypes, ctypes.wintypes as wt

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
BASE = 0x7FF751640000
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
def readable(a):
    m = MBI()
    r = k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m))
    return r == ctypes.sizeof(m) and m.State == 0x1000

# --- name pool ---
gn = BASE + R_GNAMES
cur_block = int.from_bytes(rd(gn+8,4), "little")
cursor = int.from_bytes(rd(gn+0xc,4), "little")
print("NamePool CurrentBlock=%d cursor=%d" % (cur_block, cursor))
blocks = {}
for i in range(cur_block+1):
    p = u64(rd(gn+0x10+i*8,8))
    blocks[i] = p
print("blocks:", {k: hex(v) for k,v in blocks.items()})

def entry(addr):
    raw = rd(addr, 2)
    if not raw: return None
    hdr = int.from_bytes(raw, "little")
    ln = hdr & 0x3FF
    wide = (hdr >> 10) & 1
    if hdr == 0: return ""
    if wide:
        s = rd(addr+2, ln*2)
        return s.decode("utf-16-le", "replace") if s else None
    s = rd(addr+2, ln)
    return s.decode("latin-1", "replace") if s else None

for idx in (0,1,2,10,100):
    blk = idx >> 16; off = (idx & 0xFFFF)*2
    print("idx", idx, "->", repr(entry(blocks[blk]+off)) if blk in blocks else "noblock")

# --- object array ---
gv = u64(rd(BASE + R_GOBJECTS, 8))
print("\nGObjects var = 0x%X" % gv)
vals = [u64(rd(gv+i*8,8)) for i in range(8)]
print("first 8 qwords of array:", [hex(v) for v in vals])
for ch, chunk in enumerate(vals[:2]):
    if not readable(chunk):
        print("chunk", ch, "not readable"); continue
    print("chunk", ch, "base", hex(chunk))
    for i in range(4):
        item = rd(chunk + i*24, 24)
        flags = int.from_bytes(item[0:4],"little")
        obj = u64(item[8:16])
        sn = int.from_bytes(item[16:20],"little")
        print("  item", i, "flags", flags, "obj", hex(obj), "sn", sn, "readable", readable(obj) if obj else None)
