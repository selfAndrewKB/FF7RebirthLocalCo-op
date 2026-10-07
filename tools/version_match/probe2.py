"""Structural match of a function (all displacements and immediates blanked), then list what changed."""
import sys

from pe_image import Image

OLD = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.004\ff7rebirth_.exe"
NEW = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.005\ff7rebirth_.exe"

old, new = Image(OLD), Image(NEW)
for arg in sys.argv[1:]:
    a = int(arg, 16)
    b, e = old.function_at(a)
    insns = old.disasm(b, e)
    found = None
    for n in (32, 48, 64, 96, 128, 192, 256, 384):
        end = min(b + n, e)
        pat, mask = old.masked_window(insns, b, end, loose=True)
        oh = old.search(pat, mask, limit=3)
        nh = new.search(pat, mask, limit=3)
        print(f"0x{a:X} loose len {end - b}: old {len(oh)} new {[hex(x) for x in nh]}")
        if len(oh) == 1:
            if len(nh) == 1:
                found = nh[0]
            break
        if end == e:
            break
    if not found:
        continue
    nf = new.function_at(found)
    print(f"  new function {tuple(hex(x) for x in nf) if nf else None}; old size 0x{e - b:X}")
    ninsns = new.disasm(found, found + (e - b) + 64)
    import capstone
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for oi, ni in zip(insns, ninsns):
        ob = old.read(oi.address, oi.size)
        nb = new.read(ni.address, ni.size)
        if oi.size != ni.size:
            print("  structure diverges at", hex(oi.address))
            break
        diff = [k for k in range(oi.size) if ob[k] != nb[k] and not any(o <= k < o + s for o, s in oi.masked)]
        if diff:
            o_txt = next(cs.disasm(ob, oi.address))
            n_txt = next(cs.disasm(nb, ni.address))
            print(f"  +0x{oi.address - b:X}: {o_txt.mnemonic} {o_txt.op_str}  ->  {n_txt.mnemonic} {n_txt.op_str}")
