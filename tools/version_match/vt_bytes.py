"""For each old vtable slot, does the function at new slot i+s have the same masked bytes (s = 0..2)?
Short functions compare whole; long ones their first 48 bytes. Usage: vt_bytes.py <old vt> <new vt> <slots>"""
import sys

from pe_image import Image

OLD = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.004\ff7rebirth_.exe"
NEW = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.005\ff7rebirth_.exe"
old, new = Image(OLD), Image(NEW)


def sig(img, p, n=48):
    f = img.function_at(p)
    end = min(f[1], p + n) if f else p + 16
    ins = img.disasm(p, end)
    if not f:
        # A leaf without unwind data: up to and including its first ret or jmp.
        for k in ins:
            if img.read(k.address, 1) in (b"\xc3", b"\xe9", b"\xeb"):
                end = k.address + k.size
                break
        ins = [k for k in ins if k.address < end]
    b, mask = img.masked_window(ins, p, end)
    return bytes(0 if mk else x for x, mk in zip(b, mask))


vo, vn, count = int(sys.argv[1], 16), int(sys.argv[2], 16), int(sys.argv[3])
row = []
for i in range(count):
    po = old.u64(vo + 8 * i)
    if not po or old.section_of(po) != ".text":
        print(f"{i:3}: end of vtable (0x{po or 0:X})")
        break
    so = sig(old, po)
    same = []
    for s in (0, 1, 2):
        pn = new.u64(vn + 8 * (i + s))
        if pn and new.section_of(pn) == ".text" and sig(new, pn) == so:
            same.append(s)
    row.append((i, same))
    print(f"{i:3} (+0x{8 * i:03X}) same at shift {same}  old 0x{po:X}")
