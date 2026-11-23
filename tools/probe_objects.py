import ctypes, ctypes.wintypes as wt, sys, re

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROC = 0x1F0FFF

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]

def open_pid(pid=8504):
    h = k32.OpenProcess(PROC, False, pid)
    if not h:
        raise OSError(ctypes.get_last_error(), "OpenProcess")
    return h

def mk_read(h):
    def rd(a, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]
    return rd

def u64(b): return int.from_bytes(b, "little")
def i32(b): return int.from_bytes(b, "little", signed=True)

def readable(h, a, n=8):
    m = MBI()
    r = k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m))
    return r == ctypes.sizeof(m) and m.State == 0x1000 and (m.Protect & 0x100) == 0

class NamePool:
    def __init__(self, read, gnames):
        self.read = read
        self.gnames = gnames
        head = read(gnames, 0x30)
        print("GNames head:", head.hex() if head else None)
        # try Blocks at various offsets
        for off in (0x10, 0x18, 0x20, 0x08):
            p = u64(read(gnames + off, 8) or b"\0"*8)
            if p and readable(self.read, p):
                print("  Blocks candidate at +0x%X -> 0x%X" % (off, p))
                self.blocks = off
                break
        else:
            self.blocks = 0x10
            print("  no viable Blocks pointer found; assume +0x10")

    def blocks_ptr(self, i):
        b = self.read(self.gnames + self.blocks + i * 8, 8)
        return u64(b) if b else 0

    def name(self, idx):
        block = idx >> 16
        off = (idx & 0xFFFF) * 2
        base = self.blocks_ptr(block)
        if not base:
            return None
        raw = self.read(base + off, 2)
        if not raw:
            return None
        hdr = int.from_bytes(raw, "little")
        wide = (hdr >> 15) & 1
        ln = hdr & 0x7FFF
        if wide:
            ln = hdr & 0x3FF if False else ln
            raw = self.read(base + off + 2, ln * 2)
            if not raw: return None
            return raw.decode("utf-16-le", "replace")
        raw = self.read(base + off + 2, ln)
        if not raw: return None
        return raw.decode("latin-1", "replace")

def main():
    h = open_pid()
    rd = mk_read(h)
    base = 0x7FF751640000
    # current base re-read from module list would be better
    import subprocess
    gobjs = base + 0x0AB0D3D0
    gnames = base + 0x0AD5C080
    g = rd(gobjs, 8)
    print("GObjects var @0x%X -> %s" % (gobjs, g.hex() if g else None))
    arr = u64(g)
    print("GObjects array = 0x%X" % arr)
    head = rd(arr, 0x30)
    print("head:", head.hex())
    first_gc, last_non_gc, max_not_gc = i32(head[0:4]), i32(head[4:8]), i32(head[8:12])
    print("ObjFirstGCIndex", first_gc, "LastNonGC", last_non_gc, "MaxNotGC", max_not_gc)
    objs = u64(head[0x10:0x18]); maxel = i32(head[0x18:0x1C]); numel = i32(head[0x1C:0x20])
    maxch = i32(head[0x20:0x24]); numch = i32(head[0x24:0x28])
    print("Objects=0x%X Max=%d Num=%d MaxChunks=%d NumChunks=%d" % (objs, maxel, numel, maxch, numch))
    np_ = NamePool(rd, gnames)
    # test a few chunks
    for c in range(min(numch, 3)):
        cb = u64(rd(objs + c * 8, 8))
        print("chunk", c, hex(cb))

main()
