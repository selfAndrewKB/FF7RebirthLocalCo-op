"""One-off check for the v0.62.0 traversal watch: entry bytes and vtable slots in both builds."""
import struct
import sys

import capstone

from pe_image import Image

OLD = r"G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH - Copy 1.004\End\Binaries\Win64\ff7rebirth_.exe"
NEW = r"G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64\ff7rebirth_.exe"
a, b = Image(OLD), Image(NEW)
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

FUNCS = {  # name: (1.004, 1.005)
    "TriggerEnter": (0x1409DC534, 0x140D813D0),
    "TriggerLeave": (0x1409DB844, 0x140D81588),
    "NotifyPass": (0x14125B3B0, 0x140E8ABE0),
    "LadderVt748": (0x141643238, 0x1415DC9D8),
    "LadderExecStart": (0x14132DA38, 0x141597194),
    "LadderExecEnd": (0x14132D860, 0x141597444),
    "LadderExecUpdateInput": (0x14132F4A4, 0x141594080),
    "GlideEnter": (0x143427234, 0x143144484),
    "GlideExit": (0x14342810C, 0x143145364),
    "JetEnter": (0x143432B04, 0x143151B60),
    "JetExit": (0x143433BE4, 0x143152C34),
    "ClimbTryBegin": (0x143665CE0, 0x14339B3F0),
    "JumpStandTryBegin": (0x1436663C0, 0x14339BAD0),
}
for name, (o, n) in FUNCS.items():
    bo, bn = a.read(o, 24), b.read(n, 24)
    io = next(cs.disasm(bo, o)); i_n = next(cs.disasm(bn, n))
    print(f"{name:22} 1.004 {o:#x} %16={o % 16:2} first {io.size} '{io.mnemonic} {io.op_str}' | "
          f"1.005 {n:#x} %16={n % 16:2} first {i_n.size} '{i_n.mnemonic} {i_n.op_str}'")
    print("   1.004", bo[:18].hex(" "))
    print("   1.005", bn[:18].hex(" "))

SLOTS = [  # name, 1.004 vtable, 1.004 offset, 1.005 vtable, 1.005 offset, expected fn
    ("Ladder +0x748", 0x145940748, 0x748, 0x1457A2650, 0x750, "LadderVt748"),
    ("LadderExec +0x10", 0x1458125C0, 0x10, 0x1457875F0, 0x10, "LadderExecStart"),
    ("LadderExec +0x18", 0x1458125C0, 0x18, 0x1457875F0, 0x18, "LadderExecEnd"),
    ("LadderExec +0x30", 0x1458125C0, 0x30, 0x1457875F0, 0x30, "LadderExecUpdateInput"),
    ("Glide +0x18", 0x1457953F0, 0x18, 0x145647EC0, 0x18, "GlideEnter"),
    ("Glide +0x20", 0x1457953F0, 0x20, 0x145647EC0, 0x20, "GlideExit"),
    ("Jet +0x18", 0x1457C5940, 0x18, 0x145687600, 0x18, "JetEnter"),
    ("Jet +0x20", 0x1457C5940, 0x20, 0x145687600, 0x20, "JetExit"),
]
for name, vo, oo, vn, on, fn in SLOTS:
    go, gn = a.u64(vo + oo), b.u64(vn + on)
    eo, en = FUNCS[fn]
    print(f"{name:18} 1.004 slot {vo + oo:#x} = {go:#x} {'OK' if go == eo else 'MISMATCH'} | "
          f"1.005 slot {vn + on:#x} = {gn:#x} {'OK' if gn == en else 'MISMATCH'}")

# The notify call site.
for img, site, tgt in ((a, 0x14125B205, 0x14125B3B0), (b, 0x140E8A445, 0x140E8ABE0)):
    raw = img.read(site, 5)
    rel = struct.unpack("<i", raw[1:5])[0]
    print(f"call site {site:#x} %16={site % 16} bytes {raw.hex(' ')} -> {site + 5 + rel:#x} {'OK' if site + 5 + rel == tgt else 'MISMATCH'}")

# 1.005 vtables of the chocobo field-action actors: find the vtable whose try-begin slot (+0x740 in 1.004, +0x748 in
# 1.005) holds the translated try-begin, and check the 1.004 one.
for name, vt_old, fn_old, fn_new in (("ChocoboClimb", 0x1466F6F58, 0x143665CE0, 0x14339B3F0),
                                      ("ChocoboJumpStand", 0x1466F4DC8, 0x1436663C0, 0x14339BAD0)):
    print(f"{name}: 1.004 vt+0x740 = {a.u64(vt_old + 0x740):#x} ({'OK' if a.u64(vt_old + 0x740) == fn_old else 'MISMATCH'})")
    needle = struct.pack("<Q", fn_new)
    hits, i = [], b.data.find(needle)
    while i != -1 and len(hits) < 8:
        if i % 8 == 0:
            hits.append(b.va(i))
        i = b.data.find(needle, i + 1)
    print(f"   1.005 pointers to {fn_new:#x}: {[hex(h) for h in hits]} -> vtable candidates {[hex(h - 0x748) for h in hits]}")
