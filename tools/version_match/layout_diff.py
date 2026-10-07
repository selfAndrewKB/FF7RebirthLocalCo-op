"""Compare reflected class layouts between two UE4SS object dumps (UE4SS_ObjectDump.txt).

Each property line reads `[addr] XProperty /Script/Pkg.Class:Prop [o: HEX] ...`; function parameters have a
second ':' and are skipped. For each class in both dumps: properties whose offset changed, added, removed.
Usage: python layout_diff.py <old dump> <new dump> <out.md>
"""
import collections
import re
import sys

PROP = re.compile(r"^\[[0-9A-F]+\] (\w+Property) (/Script/[^ :]+):([^ :]+) \[o: ([0-9A-F]+)\]")
CLASS = re.compile(r"^\[[0-9A-F]+\] (Class|ScriptStruct) (/Script/[^ :]+) .*?(?:\[sps: ([0-9A-F]+)\])?$")


def load(path):
    props = collections.defaultdict(dict)
    supers = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PROP.match(line)
            if m:
                props[m.group(2)][m.group(3)] = (int(m.group(4), 16), m.group(1))
                continue
            if line.startswith("[") and (" Class /Script/" in line or " ScriptStruct /Script/" in line):
                c = CLASS.match(line.rstrip("\n"))
                if c:
                    props.setdefault(c.group(2), {})
    return props


def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    common = sorted(set(old) & set(new))
    changed = []
    for c in common:
        o, n = old[c], new[c]
        moved = [(p, o[p][0], n[p][0]) for p in o if p in n and o[p][0] != n[p][0]]
        added = [p for p in n if p not in o]
        removed = [p for p in o if p not in n]
        if moved or added or removed:
            changed.append((c, moved, added, removed))
    only_old = sorted(set(old) - set(new))
    only_new = sorted(set(new) - set(old))
    end = [x for x in changed if "/Script/End" in x[0]]
    lines = [
        "# Reflected layout diff, 1.004 → 1.005",
        "",
        f"- classes/structs in both dumps: {len(common)}; with any change: **{len(changed)}** "
        f"(of which `/Script/End*`: {len(end)})",
        f"- only in the 1.004 dump: {len(only_old)}; only in the 1.005 dump: {len(only_new)} "
        "(the dumps were taken at different places, so most of these are just not loaded)",
        "- a moved property means the class (or a base class) gained or lost members before it",
        "",
        "| class | moved (first few: prop old→new) | added | removed |",
        "|---|---|---|---|",
    ]
    for c, moved, added, removed in changed:
        mv = ", ".join(f"{p} {a:#x}→{b:#x}" for p, a, b in sorted(moved, key=lambda x: x[1])[:4])
        if len(moved) > 4:
            mv += f" … ({len(moved)} moved)"
        lines.append(f"| `{c}` | {mv or '-'} | {', '.join(added[:4]) or '-'}{' …' if len(added) > 4 else ''} | "
                     f"{', '.join(removed[:4]) or '-'}{' …' if len(removed) > 4 else ''} |")
    with open(sys.argv[3], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"common {len(common)}, changed {len(changed)}, End changed {len(end)}")
    # Console: the most-used engine bases.
    for c in ("/Script/CoreUObject.Object", "/Script/Engine.Actor", "/Script/Engine.Pawn", "/Script/Engine.Character",
              "/Script/Engine.Controller", "/Script/Engine.PlayerController", "/Script/Engine.LocalPlayer",
              "/Script/Engine.GameViewportClient", "/Script/Engine.ActorComponent", "/Script/Engine.SceneComponent",
              "/Script/UMG.Widget", "/Script/UMG.UserWidget", "/Script/Engine.PlayerCameraManager"):
        st = next((x for x in changed if x[0] == c), None)
        print(f"  {c}: {'CHANGED ' + str(st[1][:3]) + ' added ' + str(st[2][:3]) if st else ('same' if c in old and c in new else 'missing in a dump')}")


if __name__ == "__main__":
    main()
