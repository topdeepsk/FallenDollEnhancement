"""盯着预览管理器，一旦出现"合法（绿色）"状态就抓一份完整快照。

用法: python tools/probe_watch.py 300
（只读，不写游戏内存。结果写到 snapshot_transition.json）
"""
import ctypes, importlib.util, json, os, struct, sys, time

spec = importlib.util.spec_from_file_location("u", os.path.join(os.path.dirname(__file__), "ue_objects_lib.py"))
u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "snapshot_transition.json")

def v3(a):
    b = u.rd(a, 24)
    if not b:
        return None
    return [round(ctypes.c_double.from_buffer_copy(b[i*8:i*8+8]).value, 2) for i in range(3)]

def find_manager():
    for idx, o in u.all_objects():
        info = u.obj(o)
        if info and info[1] == "A_ClientHPreviewManager" and not info[0].startswith("Default__"):
            return o
    return 0

def class_props(cls):
    out = {}
    p = u.u64(u.rd(cls + 0x50, 8))
    while p:
        d = u.rd(p, 0x50)
        if not d:
            break
        nm = u.name(u.i32(d[0x20:0x24]))
        off = u.i32(d[0x48:0x4c])
        fc = u.u64(d[0x08:0x10])
        fcd = u.rd(fc, 0x10) if fc else None
        t = u.name(u.i32(fcd[0:4])) if fcd else "?"
        out[nm] = (t, off)
        p = u.u64(d[0x18:0x20])
    return out

def value(obj, t, off):
    try:
        if t == "BoolProperty":
            b = u.rd(obj+off, 1); return bool(b[0]) if b else None
        if t == "IntProperty":
            b = u.rd(obj+off, 4); return u.i32(b)
        if t == "DoubleProperty":
            return round(struct.unpack('<d', u.rd(obj+off, 8))[0], 2)
        if t in ("EnumProperty", "ByteProperty"):
            b = u.rd(obj+off, 1); return b[0]
        if t.endswith("ObjectProperty"):
            v = u.u64(u.rd(obj+off, 8))
            if not v:
                return None
            info = u.obj(v)
            return {"addr": hex(v), "class": info[1], "name": info[0]} if info else {"addr": hex(v)}
        if t in ("ArrayProperty", "StructProperty", "MapProperty", "SetProperty"):
            return {"raw": u.rd(obj+off, 24).hex()}
    except Exception as e:
        return {"error": str(e)}
    return None

def snapshot(mgr, props):
    return {nm: value(mgr, t, off) for nm, (t, off) in props.items()}

def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    mgr = find_manager()
    if not mgr:
        print("找不到 A_ClientHPreviewManager")
        return
    cls = u.u64(u.rd(mgr + 0x10, 8))
    props = class_props(cls)
    print("manager=0x%X props=%d 监视 %.0fs" % (mgr, len(props), seconds))

    history = []
    deadline = time.time() + seconds
    captured = None
    while time.time() < deadline:
        s = snapshot(mgr, props)
        valid = bool(s.get("isSnappingItem") or s.get("isSnappingWall") or s.get("FloorPreviewZ"))
        history.append(s)
        history = history[-4:]
        if valid and captured is None:
            captured = {"manager": hex(mgr), "state": s, "history": history[:-1], "ghost": None}
            group = u.u64(u.rd(mgr + props["preview"][1], 8)) if "preview" in props else 0
            if group:
                tgt = u.u64(u.rd(group + 0x2C0, 8))
                if tgt:
                    captured["ghost"] = v3(tgt + 0x210)
            with open(OUT, "w", encoding="utf-8") as f:
                json.dump(captured, f, ensure_ascii=False, indent=1)
            print("抓到合法状态: item=%s wall=%s floor=%s ghost=%s" % (
                s.get("isSnappingItem"), s.get("isSnappingWall"), s.get("FloorPreviewZ"),
                captured["ghost"]))
            break
        time.sleep(0.02)

    if captured is None:
        last = history[-1] if history else {}
        print("超时：未出现合法状态。最后一次 item=%s wall=%s floor=%s" % (
            last.get("isSnappingItem"), last.get("isSnappingWall"), last.get("FloorPreviewZ")))

main()
