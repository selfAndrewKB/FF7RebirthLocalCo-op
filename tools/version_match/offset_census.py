"""Census of the mod's integer constants (field offsets, vtable offsets, sizes) for a layout audit.

For each `constexpr <int type> kName = <value>;` in the mod: file, line, its trailing comment and the nearest section
comment above it, and the reflected properties (from both UE4SS object dumps) that sat at that offset in 1.004, with
their 1.005 offsets. A reflected match is a lead, not proof: the same offset exists in many classes.
Usage: python offset_census.py <src> <old dump> <new dump> <out.json> <out.md>
"""
import collections
import json
import os
import re
import sys

from layout_diff import load

CONST = re.compile(r"constexpr\s+(?:std::)?(u?int\d+_t|size_t|ptrdiff_t|uintptr_t)\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;\s*(?://\s*(.*))?")
SECTION = re.compile(r"^\s*//\s*(.+)$")


def main():
    src, old_dump, new_dump, out_json, out_md = sys.argv[1:6]
    old, new = load(old_dump), load(new_dump)
    by_offset = collections.defaultdict(list)  # old offset -> [(class, prop, new offset or None)]
    for cls, props in old.items():
        for p, (off, _) in props.items():
            n = new.get(cls, {}).get(p)
            by_offset[off].append((cls.split(".")[-1], p, n[0] if n else None))
    rows = []
    for root, _, files in os.walk(src):
        for fn in files:
            if not fn.endswith((".h", ".cpp")):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, src).replace("\\", "/")
            lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
            section = ""
            for i, line in enumerate(lines, 1):
                s = SECTION.match(line)
                if s and not line.strip().startswith("///") and len(s.group(1)) < 90:
                    section = s.group(1).strip()
                m = CONST.search(line)
                if not m:
                    continue
                value = int(m.group(3), 0)
                if value >= 0x14000000:  # an address, handled by the matcher
                    continue
                cands = by_offset.get(value, [])
                moved = [c for c in cands if c[2] is not None and c[2] != value]
                rows.append({"file": rel, "line": i, "name": m.group(2), "type": m.group(1), "value": hex(value),
                             "comment": (m.group(4) or "").strip(), "section": section,
                             "reflected_at_offset": len(cands), "reflected_moved": len(moved),
                             "examples_moved": [f"{c}.{p} -> {hex(n)}" for c, p, n in moved[:5]],
                             "examples_same": [f"{c}.{p}" for c, p, n in cands if n == value][:5]})
    json.dump(rows, open(out_json, "w"), indent=1)
    md = ["# Offset-constant census (leads for the 1.005 layout audit)", "",
          f"{len(rows)} integer constants below 0x14000000. `moved` = reflected properties at that 1.004 offset whose "
          "1.005 offset differs (any class). A lead, not proof.", "",
          "| file:line | name | value | section / comment | reflected at offset (moved) | moved examples |",
          "|---|---|---|---|---|---|"]
    for r in rows:
        md.append(f"| {r['file']}:{r['line']} | {r['name']} | {r['value']} | {r['section'][:40]} / {r['comment'][:50]} | "
                  f"{r['reflected_at_offset']} ({r['reflected_moved']}) | {'; '.join(r['examples_moved'][:3])} |")
    open(out_md, "w", encoding="utf-8").write("\n".join(md) + "\n")
    print(len(rows), "constants;", sum(1 for r in rows if r["reflected_moved"]), "with a moved reflected property at their offset")


if __name__ == "__main__":
    main()
