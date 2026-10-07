"""Map a vtable slot by slot between builds: translate each old slot's function, find it in the new vtable.
Usage: python vt_compare.py <old vtable> <new vtable> <slots>"""
import sys

from match import Matcher
from pe_image import Image

OLD = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.004\ff7rebirth_.exe"
NEW = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.005\ff7rebirth_.exe"

old, new = Image(OLD), Image(NEW)
m = Matcher(old, new)
vo, vn, count = int(sys.argv[1], 16), int(sys.argv[2], 16), int(sys.argv[3])
new_slots = [new.u64(vn + 8 * k) for k in range(count + 8)]
prev = None
for i in range(count):
    p = old.u64(vo + 8 * i)
    r = m.match_code(p) if p and old.section_of(p) == ".text" else None
    if not r:
        print(f"  {i:3} (+0x{8 * i:03X}) 0x{p:X}: no match")
        continue
    pn = p - r.old + r.new
    js = [j for j, q in enumerate(new_slots) if q == pn]
    shift = (js[0] - i) if js else None
    near = min(js, key=lambda j: abs(j - i)) if js else None
    s = (near - i) if near is not None else None
    if s != prev:
        print(f"  {i:3} (+0x{8 * i:03X}) -> new {near} (+0x{8 * near:03X}) shift {s}" if near is not None
              else f"  {i:3}: function 0x{pn:X} not in new vtable")
        prev = s
print("done")
