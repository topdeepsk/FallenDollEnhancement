"""把若干 actor 整体平移（根组件 + 所有子组件各写一次），用于临时移开障碍物。

用法:
  python tools/probe_move.py list                 # 列出红点附近候选
  python tools/probe_move.py move                # 移开候选并记录到 moved.json
  python tools/probe_move.py restore             # 按记录还原
"""
import ctypes, importlib.util, json, math, os, struct, sys

spec = importlib.util.spec_from_file_location("u", os.path.join(os.path.dirname(__file__), "ue_objects_lib.py"))
u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)

RED = (928.4, 1618.6, 283.8)
RADIUS = 170.0
DELTA = (0.0, 2000.0, 0.0)
RECORD = os.path.join(os.path.dirname(__file__), "..", "moved.json")

def v3(a):
    b = u.rd(a, 24)
    if not b:
        return None
    v = tuple(ctypes.c_double.from_buffer_copy(b[i*8:i*8+8]).value for i in range(3))
    return v if all(math.isfinite(x) and abs(x) < 1e6 for x in v) else None

def write3(a, v):
    return u.wr(a, struct.pack('<3d', *v))

def shift(addr, d):
    v = v3(addr)
    if not v:
        return False
    return write3(addr, (v[0]+d[0], v[1]+d[1], v[2]+d[2]))

def children(actor):
    out = []
    for idx, o in u.all_objects():
        b = u.rd(o, 0x28)
        if not b:
            continue
        if u.u64(b[0x20:0x28]) != actor:
            continue
        info = u.obj(o)
        if info and info[1].endswith("Component"):
            out.append(o)
    return out

def move_actor(actor, delta):
    root = u.u64(u.rd(actor + 0x1b8, 8) or b'\0'*8)
    n = 0
    for comp in children(actor):
        if comp == root:
            continue
        if shift(comp + 0x210, delta) or shift(comp + 0x148, delta):
            n += 1
    if root:
        shift(root + 0x210, delta)
        shift(root + 0x148, delta)
        n += 1
    return n

def candidates():
    out = []
    for idx, o in u.all_objects():
        try:
            b = u.rd(o, 0x28)
            if not b:
                continue
            cls = u.u64(b[0x10:0x18])
            cb = u.rd(cls, 0x20) if cls else None
            if not cb:
                continue
            cn = u.name(u.i32(cb[0x18:0x1c]))
            if not (cn.endswith("_C") or cn in ("StaticMeshActor",)):
                continue
            root = u.u64(u.rd(o + 0x1b8, 8) or b'\0'*8)
            if not root:
                continue
            p = v3(root + 0x210)
            if not p:
                continue
            d = math.dist(p, RED)
            if d < RADIUS:
                out.append((round(d, 1), cn, u.obj(o)[0], o))
        except Exception:
            continue
    out.sort()
    return out

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for d, cn, nm, o in candidates():
            print("%7.1f %-16s %-14s 0x%X" % (d, cn, nm, o))
        return
    if cmd == "move":
        recs = []
        for d, cn, nm, o in candidates():
            n = move_actor(o, DELTA)
            recs.append({"actor": hex(o), "class": cn, "name": nm, "components": n})
            print("moved %-14s %-16s dist=%.1f comps=%d" % (nm, cn, d, n))
        json.dump(recs, open(RECORD, "w"), indent=1)
        print("recorded", len(recs))
        return
    if cmd == "restore":
        recs = json.load(open(RECORD))
        neg = (-DELTA[0], -DELTA[1], -DELTA[2])
        for r in recs:
            move_actor(int(r["actor"], 16), neg)
        print("restored", len(recs))

main()
