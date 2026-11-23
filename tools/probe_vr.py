"""自动定位进程与基址，然后列出 VR 相关类，以及各预览管理器上的输入事件函数。

用法：
    python tools/probe_vr.py            # 只列 VR 类名
    python tools/probe_vr.py funcs      # 额外列出 Preview* 类的 InpActEvt/Click/Trigger 函数

不写任何内存，只读。
"""
import ctypes
import ctypes.wintypes as wt
import re
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi")


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


R_GOBJECTS = 0x0AB0D3D0
R_GNAMES = 0x0AD5C080
CHUNK = 65536
ITEM = 24


def module_base(pid, index=0):
    h = k32.OpenProcess(0x410, False, pid)
    if not h:
        return 0
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, None, 0, ctypes.byref(need), 3)
    arr = (ctypes.c_void_p * (need.value // 8))()
    psapi.EnumProcessModulesEx(h, arr, need.value, ctypes.byref(need), 3)
    v = int(arr[index]) if len(arr) > index else 0
    k32.CloseHandle(h)
    return v


class Rt:
    def __init__(self, pid, base):
        self.h = k32.OpenProcess(0x1F0FFF, False, pid)
        self.base = base
        self.gn = base + R_GNAMES
        self._blocks = {}
        self._ncache = {}
        self._clscache = {}
        self.table = self.u64r(base + R_GOBJECTS)

    def rd(self, a, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)):
            return None
        if got.value != n:
            return None
        return buf.raw

    def u64r(self, a):
        b = self.rd(a, 8)
        return int.from_bytes(b, "little") if b else 0

    def block(self, i):
        if i not in self._blocks:
            self._blocks[i] = self.u64r(self.gn + 0x10 + i * 8)
        return self._blocks[i]

    def name(self, idx):
        if idx in self._ncache:
            return self._ncache[idx]
        b = self.block(idx >> 16)
        off = (idx & 0xFFFF) * 2
        v = "?"
        if b:
            raw = self.rd(b + off, 2)
            if raw:
                ln = int.from_bytes(raw, "little") >> 6
                if 0 < ln <= 512:
                    s = self.rd(b + off + 2, ln)
                    if s:
                        if all(32 <= c < 127 for c in s):
                            v = s.decode("ascii")
                        else:
                            try:
                                v = (self.rd(b + off + 2, ln * 2) or b"").decode("utf-16-le")
                            except UnicodeDecodeError:
                                v = s.decode("latin-1")
        self._ncache[idx] = v
        return v

    def obj(self, addr):
        b = self.rd(addr, 0x28)
        if not b:
            return None
        cls = int.from_bytes(b[0x10:0x18], "little")
        ni = int.from_bytes(b[0x18:0x1C], "little", signed=True)
        num = int.from_bytes(b[0x1C:0x20], "little", signed=True)
        if cls not in self._clscache:
            cb = self.rd(cls, 0x20)
            self._clscache[cls] = self.name(int.from_bytes(cb[0x18:0x1C], "little", signed=True)) if cb else "?"
        return self.name(ni), self._clscache[cls], cls, num

    def all_objects(self):
        for ci in range(16):
            base = self.u64r(self.table + ci * 8)
            if not base:
                continue
            data = self.rd(base, CHUNK * ITEM)
            if not data:
                continue
            for i in range(CHUNK):
                o = int.from_bytes(data[i * ITEM + 8:i * ITEM + 16], "little")
                if o:
                    yield ci * CHUNK + i, o


def pick_process():
    """挑出 GNames 自检通过的那个进程（索引 0 应为 None）。"""
    out = []
    for pid in (20464, 24528):
        base = module_base(pid)
        if not base:
            continue
        try:
            rt = Rt(pid, base)
        except Exception:
            continue
        n0 = rt.name(0) if rt.block(0) else "?"
        out.append((pid, base, n0, rt))
    return out


def walk_list(rt, head_addr):
    """遍历 UClass 的 Children/ChildProperties 链表，返回 [(name, addr)]。"""
    res = []
    p = rt.u64r(head_addr)
    n = 0
    while p and n < 2000:
        d = rt.rd(p, 0x50)
        if not d:
            break
        nm = rt.name(int.from_bytes(d[0x20:0x24], "little", signed=True))
        res.append((nm, p))
        p = int.from_bytes(d[0x18:0x20], "little")
        n += 1
    return res


def main():
    want_funcs = len(sys.argv) > 1 and sys.argv[1] == "funcs"
    cands = pick_process()
    for pid, base, n0, rt in cands:
        print("pid=%d base=0x%X GNames[0]=%r GObjects=0x%X" % (pid, base, n0, rt.table))

    live = [c for c in cands if c[2] == "None"]
    if not live:
        print("没有自检通过的进程")
        return
    pid, base, _, rt = live[0]
    print("=> 使用 pid=%d\n" % pid)

    classes = {}
    for idx, o in rt.all_objects():
        info = rt.obj(o)
        if info and info[1] in ("ASClass", "Class", "BlueprintGeneratedClass"):
            classes.setdefault(info[0], o)

    vr = sorted(k for k in classes if re.search(r"vr", k, re.I))
    print("== 名字含 VR 的类（%d）==" % len(vr))
    for k in vr:
        print("   ", k)

    prev = sorted(k for k in classes if re.search(r"(hpreview|previewmanager)", k, re.I))
    print("\n== 预览管理器类（%d）==" % len(prev))
    for k in prev:
        print("   ", k, "inst-super?", "ASClass" if rt.obj(classes[k])[1] == "ASClass" else "")

    if want_funcs:
        kw = re.compile(r"(InpActEvt|InpAxis|Click|Trigger|Confirm|Grip|Place)", re.I)
        for k in prev:
            c = classes[k]
            print("\n#### %s @0x%X" % (k, c))
            for nm, addr in walk_list(rt, c + 0x48):
                if kw.search(nm):
                    print("   F %s" % nm)
            for nm, addr in walk_list(rt, c + 0x50):
                print("   P %s" % nm)


if __name__ == "__main__":
    main()
