"""读取某个 AS 类的属性表（含父类）与实例当前值。

用法：
    python tools/probe_props.py A_PawnModeManager
    python tools/probe_props.py A_ClientHController A_ClientHManager
"""
import importlib.util
import os
import re
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("p", os.path.join(_here, "probe_vr.py"))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

TYPED = ("BoolProperty", "ObjectProperty", "StructProperty", "FloatProperty", "DoubleProperty",
         "IntProperty", "ByteProperty", "EnumProperty", "NameProperty", "StrProperty")


def ftype(rt, field_addr):
    b = rt.rd(field_addr, 0x10)
    if not b:
        return "?"
    return rt.name(int.from_bytes(b[0x0:0x4], "little", signed=True))


def walk_props(rt, cls):
    """返回 [(name, type, offset, elemSize)]，沿父类链，父类先压栈。"""
    out = []
    c = cls
    seen = set()
    while c and c not in seen:
        seen.add(c)
        head = rt.u64r(c + 0x50)
        n = 0
        while head and n < 400:
            d = rt.rd(head, 0x58)
            if not d:
                break
            nm = rt.name(int.from_bytes(d[0x20:0x24], "little", signed=True))
            off = int.from_bytes(d[0x48:0x4C], "little", signed=True)
            es = int.from_bytes(d[0x34:0x38], "little", signed=True)
            out.append((nm, ftype(rt, int.from_bytes(d[0x08:0x10], "little")), off, es))
            head = int.from_bytes(d[0x18:0x20], "little")
            n += 1
        c = rt.u64r(c + 0x40)
    return out


def read_val(rt, obj, typ, off):
    if typ == "BoolProperty":
        b = rt.rd(obj + off, 1)
        return ("true" if b[0] else "false") if b else "-"
    if typ in ("FloatProperty",):
        b = rt.rd(obj + off, 4)
        return ("%.2f" % struct.unpack("<f", b)[0]) if b else "-"
    if typ in ("DoubleProperty",):
        b = rt.rd(obj + off, 8)
        return ("%.2f" % struct.unpack("<d", b)[0]) if b else "-"
    if typ in ("IntProperty", "EnumProperty", "ByteProperty"):
        b = rt.rd(obj + off, 4)
        return str(int.from_bytes(b, "little", signed=True)) if b else "-"
    if typ == "ObjectProperty":
        b = rt.rd(obj + off, 8)
        if not b:
            return "-"
        v = int.from_bytes(b, "little")
        if not v:
            return "null"
        info = rt.obj(v)
        return (info[1] + " / " + info[0]) if info else ("0x%X" % v)
    return ""


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    pid, base, _, rt = [c for c in p.pick_process() if c[2] == "None"][0]
    classes = {}
    insts = {}
    for idx, o in rt.all_objects():
        info = rt.obj(o)
        if not info:
            continue
        if info[1] in ("ASClass", "Class", "BlueprintGeneratedClass"):
            classes.setdefault(info[0], o)
        elif not info[0].startswith("Default__"):
            insts.setdefault(info[2], o)

    for name in sys.argv[1:]:
        cls = classes.get(name)
        print("#### %s @0x%X" % (name, cls or 0))
        if not cls:
            print("   类不存在")
            continue
        props = walk_props(rt, cls)
        obj = insts.get(cls)
        print("   实例=0x%X 属性=%d" % (obj or 0, len(props)))
        for nm, typ, off, es in props:
            if typ not in TYPED:
                continue
            val = read_val(rt, obj, typ, off) if obj else ""
            print("   +0x%03X %-18s %-30s %s" % (off, typ, nm, val))
        print()


if __name__ == "__main__":
    main()
