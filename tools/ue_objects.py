"""枚举 UObject 表（不依赖 FUObjectArray 头，直接按块指针表走）。

用法:
  python tools/ue_objects.py "正则"        # 过滤 对象名/类名
  python tools/ue_objects.py --all 正则    # 同时打印类名统计
"""
import ctypes, ctypes.wintypes as wt, sys, re, collections

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
BASE = 0x7FF751640000
R_GOBJECTS = 0x0AB0D3D0
R_GNAMES   = 0x0AD5C080
CHUNK = 65536
ITEM = 24

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]

h = k32.OpenProcess(0x1F0FFF, False, 8504)
assert h, "OpenProcess 失败（需要管理员）"

def rd(a, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw

def u64(b): return int.from_bytes(b, "little")
def i32(b): return int.from_bytes(b, "little", signed=True)

gn = BASE + R_GNAMES
TABLE = u64(rd(BASE + R_GOBJECTS, 8))
_blocks = {}
_ncache = {}

def block(i):
    if i not in _blocks:
        _blocks[i] = u64(rd(gn + 0x10 + i * 8, 8) or b"\0" * 8)
    return _blocks[i]

def name(idx):
    v = _ncache.get(idx)
    if v is not None:
        return v
    b = block(idx >> 16)
    off = (idx & 0xFFFF) * 2
    v = "?"
    if b:
        raw = rd(b + off, 2)
        if raw:
            hdr = int.from_bytes(raw, "little")
            ln = hdr >> 6
            if 0 < ln <= 512:
                s = rd(b + off + 2, ln)
                if s:
                    if all(32 <= c < 127 for c in s):
                        v = s.decode("ascii")
                    else:
                        s2 = rd(b + off + 2, ln * 2)
                        try:
                            t = s2.decode("utf-16-le") if s2 else None
                        except UnicodeDecodeError:
                            t = None
                        v = t if t and all(32 <= ord(c) < 0xFFFD for c in t) else s.decode("latin-1")
    _ncache[idx] = v
    return v

_clscache = {}

def obj(addr):
    b = rd(addr, 0x28)
    if not b:
        return None
    cls = u64(b[0x10:0x18])
    ni = i32(b[0x18:0x1C]); num = i32(b[0x1C:0x20])
    if cls not in _clscache:
        cb = rd(cls, 0x20)
        _clscache[cls] = name(i32(cb[0x18:0x1C])) if cb else "?"
    return name(ni), _clscache[cls], cls, num

def chunks():
    out = []
    for i in range(16):
        p = u64(rd(TABLE + i * 8, 8) or b"\0" * 8)
        if p:
            out.append(p)
    return out

def main():
    pat = sys.argv[-1] if len(sys.argv) > 1 else "."
    kw = re.compile(pat, re.I)
    cls_count = collections.Counter()
    total = 0
    out = []
    for ci, base in enumerate(chunks()):
        data = rd(base, CHUNK * ITEM)
        if not data:
            print("chunk", ci, "read fail")
            continue
        for i in range(CHUNK):
            o = u64(data[i*ITEM+8:i*ITEM+16])
            if not o:
                continue
            info = obj(o)
            if not info:
                continue
            total += 1
            nm, cn, cls, num = info
            cls_count[cn] += 1
            if kw.search(nm) or kw.search(cn):
                out.append((o, ci*CHUNK+i, cn, nm))
    for o, idx, cn, nm in out:
        print("0x%-12X idx=%-6d %-32s %s" % (o, idx, cn, nm))
    print("---- objects=%d  matches=%d" % (total, len(out)))
    print("top classes:")
    for c, n in cls_count.most_common(25):
        print("  %5d  %s" % (n, c))

main()
