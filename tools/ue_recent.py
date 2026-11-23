"""列出最近创建的 N 个 UObject（按 InternalIndex 倒序），用于观察"刚发生的操作"。

用法: python tools/ue_recent.py 120
"""
import sys, importlib.util, os
spec = importlib.util.spec_from_file_location("u", os.path.join(os.path.dirname(__file__), "ue_objects_lib.py"))
u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
items = []
for ci, base in enumerate(u.chunks()):
    data = u.rd(base, u.CHUNK * u.ITEM)
    if not data:
        continue
    for i in range(u.CHUNK):
        o = u.u64(data[i*u.ITEM+8:i*u.ITEM+16])
        if o:
            items.append((ci*u.CHUNK+i, o))
items.sort(reverse=True)
print("total", len(items))
for idx, o in items[:N]:
    info = u.obj(o)
    if info:
        nm, cn, cls, num = info
        print("%-6d 0x%-12X %-34s %s" % (idx, o, cn, nm))
