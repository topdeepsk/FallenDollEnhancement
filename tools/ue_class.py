"""读取 UClass/UStruct 的父类链、字段链与函数链（运行时，不依赖 dump）。

用法: python tools/ue_class.py <类名子串> [--fields] [--funcs]
"""
import sys, importlib.util, os

spec = importlib.util.spec_from_file_location("u", os.path.join(os.path.dirname(__file__), "ue_objects_lib.py"))
u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)

def read_name_at(addr, name_off=0x18):
    b = u.rd(addr + name_off, 8)
    if not b:
        return None
    return u.name(u.i32(b[0:4]))

def class_name_of(addr):
    b = u.rd(addr + 0x10, 8)
    if not b:
        return None
    return read_name_at(u.u64(b))

def walk_next(field):
    seen = 0
    out = []
    while field and seen < 4000:
        nm = read_name_at(field)
        cn = class_name_of(field)
        out.append((nm, cn, field))
        nb = u.rd(field + 0x28, 8)
        if not nb:
            break
        field = u.u64(nb)
        seen += 1
    return out

def main():
    pat = sys.argv[1]
    want_fields = "--fields" in sys.argv
    want_funcs = "--funcs" in sys.argv
    cands = u.find_class(pat)
    if not cands:
        print("找不到类:", pat)
        return
    for cname, caddr in cands:
        print("### %s @ 0x%X" % (cname, caddr))
        # 逐偏移探测 SuperStruct
        for off in (0x38, 0x40, 0x48, 0x30):
            b = u.rd(caddr + off, 8)
            if not b:
                continue
            p = u.u64(b)
            if p and class_name_of(p) == "Class":
                print("  SuperStruct @+0x%X -> %s" % (off, read_name_at(p)))
                super_off = off
                break
        else:
            print("  SuperStruct 探测失败")
            continue
        # 逐偏移探测 Children
        for off in (0x40, 0x48, 0x50, 0x38):
            if off == super_off:
                continue
            b = u.rd(caddr + off, 8)
            if not b:
                continue
            p = u.u64(b)
            if p and class_name_of(p) in ("Function", "Property", "BoolProperty", "ObjectProperty", "StructProperty", "ArrayProperty", "ByteProperty", "EnumProperty", "IntProperty", "FloatProperty", "NameProperty", "StrProperty", "TextProperty", "ClassProperty", "MulticastInlineDelegateProperty", "DelegateProperty"):
                print("  Children @+0x%X" % off)
                fields = walk_next(p)
                for nm, cn, a in fields:
                    if cn == "Function" and not want_funcs:
                        continue
                    if cn != "Function" and not want_fields:
                        continue
                    print("     %-22s %-28s @0x%X" % (cn, nm, a))
                break
        print()

main()
