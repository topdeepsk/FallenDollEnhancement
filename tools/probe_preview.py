"""快照"动作放置预览"相关对象的运行时状态，用于对比红色/绿色两种状态。

用法：
    python tools/probe_preview.py red     # 预览是红色时执行
    python tools/probe_preview.py green   # 挪到绿色位置后再执行
    python tools/probe_preview.py diff    # 对比两次快照

输出 snapshot_<tag>.json，列出 A_ClientHPreviewManager / A_ClientHPreviewGroup 的
每个脚本属性（名字、类型、偏移、当前值、引用的对象），不写游戏内存。
"""
import ctypes, ctypes.wintypes as wt, importlib.util, os, sys, json

spec = importlib.util.spec_from_file_location("u", os.path.join(os.path.dirname(__file__), "ue_objects_lib.py"))
u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)

SKIP_STATIC = "CDO"  # Default__ 开头的模板对象

def class_obj(name):
    for idx, o in u.all_objects():
        info = u.obj(o)
        if info and info[1] == "ASClass" and info[0] == name:
            return o
    return 0

def props(cls):
    p = u.u64(u.rd(cls + 0x50, 8) or b"\0"*8)
    out = []
    n = 0
    while p and n < 500:
        d = u.rd(p, 0x50)
        if not d:
            break
        fclass = u.u64(d[0x08:0x10])
        nxt = u.u64(d[0x18:0x20])
        nm = u.name(u.i32(d[0x20:0x24]))
        off = u.i32(d[0x48:0x4C])
        fcd = u.rd(fclass, 0x10) if fclass else None
        ftype = u.name(u.i32(fcd[0:4])) if fcd else "?"
        esz = u.i32(d[0x34:0x38])
        out.append((nm, ftype, off, esz))
        p = nxt
        n += 1
    return out

def read_value(obj, ftype, off, esz):
    if ftype in ("BoolProperty",):
        b = u.rd(obj + off, 1)
        return bool(b[0]) if b else None
    if ftype in ("IntProperty", "Int32Property", "UInt32Property"):
        b = u.rd(obj + off, 4)
        return u.i32(b) if b else None
    if ftype in ("FloatProperty",):
        b = u.rd(obj + off, 4)
        return ctypes.c_float.from_buffer_copy(b).value if b else None
    if ftype in ("DoubleProperty",):
        b = u.rd(obj + off, 8)
        return ctypes.c_double.from_buffer_copy(b).value if b else None
    if ftype in ("EnumProperty", "ByteProperty"):
        b = u.rd(obj + off, 1)
        return b[0] if b else None
    if ftype in ("NameProperty",):
        b = u.rd(obj + off, 8)
        return {"name": u.name(u.i32(b[0:4])), "number": u.i32(b[4:8])} if b else None
    if ftype.endswith("ObjectProperty") or ftype == "ClassProperty":
        b = u.rd(obj + off, 8)
        if not b:
            return None
        v = u.u64(b)
        if v == 0:
            return None
        info = u.obj(v)
        return {"addr": "0x%X" % v, "class": info[1], "name": info[0]} if info else {"addr": "0x%X" % v}
    if ftype in ("StrProperty", "TextProperty"):
        b = u.rd(obj + off, 8)
        if not b:
            return None
        return {"ptr": "0x%X" % u.u64(b)}
    if ftype.endswith("Property"):
        b = u.rd(obj + off, min(esz, 16))
        return {"raw": b.hex()} if b else None
    return None

def dump_instance(obj, cls_name, out):
    cls = u.u64(u.rd(obj + 0x10, 8))
    rec = {"addr": "0x%X" % obj, "class": cls_name, "props": {}}
    for nm, ftype, off, esz in props(cls):
        try:
            rec["props"][nm] = {"type": ftype, "offset": off, "value": read_value(obj, ftype, off, esz)}
        except Exception as e:
            rec["props"][nm] = {"type": ftype, "offset": off, "error": str(e)}
    out.append(rec)
    return rec

def instances(cls_name):
    res = []
    for idx, o in u.all_objects():
        info = u.obj(o)
        if info and info[1] == cls_name:
            res.append((idx, o, info[0]))
    return res

def snapshot(tag):
    out = []
    for cls_name in ("A_ClientHPreviewManager", "A_ClientHPreviewGroup"):
        for idx, o, nm in instances(cls_name):
            rec = dump_instance(o, cls_name, out)
            rec["instance_name"] = nm
    path = os.path.join(os.path.dirname(__file__), "..", "snapshot_%s.json" % tag)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("written", os.path.abspath(path), "objects:", len(out))
    for rec in out:
        print(" ", rec["class"], rec["instance_name"], rec["addr"])
        for k, v in rec["props"].items():
            if v.get("value") not in (None, False, 0, 0.0):
                print("     %-28s %-24s %s" % (k, v["type"], v["value"]))

def diff():
    a = json.load(open(os.path.join(os.path.dirname(__file__), "..", "snapshot_red.json"), encoding="utf-8"))
    b = json.load(open(os.path.join(os.path.dirname(__file__), "..", "snapshot_green.json"), encoding="utf-8"))
    for ra, rb in zip(a, b):
        if ra["class"] != rb["class"]:
            continue
        print("###", ra["class"], ra["instance_name"])
        keys = set(ra["props"]) | set(rb["props"])
        for k in sorted(keys):
            va = ra["props"].get(k, {}).get("value")
            vb = rb["props"].get(k, {}).get("value")
            if va != vb:
                print("  %-28s red=%s  green=%s" % (k, va, vb))

def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "red"
    if arg == "diff":
        diff()
    else:
        snapshot(arg)

main()
