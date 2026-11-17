"""Debug one address: how its function's masked prefix matches in old and new as it grows."""
import sys

from pe_image import Image, build_exe

OLD = build_exe("1.004")
NEW = build_exe("1.005")

old, new = Image(OLD), Image(NEW)
for arg in sys.argv[1:]:
    a = int(arg, 16)
    f = old.function_at(a)
    print(f"== 0x{a:X} function {tuple(hex(x) for x in f) if f else None}")
    b, e = f if f else (a, a + 0x100)
    insns = old.disasm(b, e)
    for start in (b, a) if a != b else (b,):
        for n in (16, 24, 32, 48, 64, 96, 128):
            if start + n > e:
                break
            pat, mask = old.masked_window(insns, start, start + n)
            oh = old.search(pat, mask, limit=3)
            nh = new.search(pat, mask, limit=3)
            print(f"  from 0x{start:X} len {n:3}: old {len(oh)} new {len(nh)} {[hex(x) for x in nh]}")
    print("  old bytes:", old.read(b, 48).hex(" "))
