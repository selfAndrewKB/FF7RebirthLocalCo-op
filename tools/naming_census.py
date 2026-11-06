"""Every name in the code that still means "player 2": the ledger of the seats rename and its check.

Reads src/ and tests/, strips comments and string literals, and finds the identifiers of the families below. Comments
and strings are counted apart (their wording is another stage's), so a research file named RE-p2-*.md or a console
reading named p2.state never counts as a name in the code.

    python -I tools/naming_census.py                 the summary, by family and by folder
    python -I tools/naming_census.py --ledger FILE   the ledger as Markdown
    python -I tools/naming_census.py --left          the names still there, one per line (exit 1 if any)
    python -I tools/naming_census.py --group NAME    one group's files and the names in each, without the dev watches

Tests are counted but their own fixtures (a test's kTwo and kThree, which name players 2 and 3 on purpose) are not
names to change; a test follows the module it tests.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

FAMILIES = {
    "PlayerTwo": r"\w*[Pp]layerTwo\w*",
    "Two": r"\w*Two(?![a-z])\w*|\btwo(?![a-z])\w*|\w+_two(?![a-z])\w*",
    "Second": r"\w*[Ss]econd(?:Image|Player|View|Pass|Family)\w*",
    "P2": r"\w*P2\w*|\bp2\w*|\w+_p2\w*",
    "Three": r"\w*Three(?![a-z])\w*|\bthree(?![a-z])\w*|\w+_three(?![a-z])\w*",
}

# Names that do not mean a player: the engine's own, or a "second"/"two" of something else. Each says why.
KEEP = {
    "TwoPlayerSplitscreenLayout": "the engine's setting",
    "ThreePlayerSplitscreenLayout": "the engine's setting",
    "ETwoPlayerSplitScreenType": "the engine's enum",
    "EThreePlayerSplitScreenType": "the engine's enum",
    "kLayerTwo": "move-type layer 2, not a player",
    "TwoPlayerLayout": "the engine's layout for two players: a count",
    "ThreePlayerLayout": "the engine's layout for three players: a count",
    "SetTwoPlayerLayout": "a count", "SetThreePlayerLayout": "a count",
    "ReadTwoPlayerLayout": "a count", "ReadThreePlayerLayout": "a count",
    "WriteTwoPlayerLayout": "a count", "WriteThreePlayerLayout": "a count",
    "kTwoPlayerLayout": "a count", "kThreePlayerLayout": "a count",
    "kThreeLayoutName": "a count", "threeLayout_": "a count",
    "kTwoPlayerSplitscreenLayoutOffset": "the engine's field", "kThreePlayerSplitscreenLayoutOffset": "the engine's field",
    "kThreeColumns": "a count of columns", "threeColumns": "a count of columns", "twoColumns": "a count of columns",
    "kPoolTwo": "render pool 2, not a player",
    "kPoolTwoVertexBuffer": "render pool 2", "kPoolTwoIndexBuffer": "render pool 2",
    "kBrakeTwo": "the brake move's second phase", "kBrakeTwoSlot": "the brake move's second phase",
    "g_brakeTwoSlot": "the brake move's second phase", "BrakeTwo": "the brake move's second phase",
    "kMeasureFirstTwo": "the measure's first two instructions: a count",
}

# The same, for a name that means a player elsewhere: kept in the one file named with it.
KEEP_IN = {
    ("src/hud/passes/menu_buffers.h", "two"): "the number 2 the game's constructor writes",
    ("src/platform/memory_watch.cpp", "three"): "the adapter's interface, version 3",
}

# The folder groups the rename is split into.
GROUPS = {
    "battle": "battle", "scopes": "battle",
    "camera": "camera", "input": "camera",
    "feedback": "feedback",
    "rides": "rides", "traversal": "rides",
    "world": "world",
    "session": "session", "players": "session", "control": "session", "dev": "session", "modules": "session",
    "hud": "hud", "views": "hud", "renderer": "hud", "platform": "hud", "facts": "hud",
    "tests": "tests",
}


def strip(text: str) -> tuple[str, list[str], list[str]]:
    """The code with comments and string literals blanked, and those comments and strings."""
    out, comments, strings = [], [], []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            comments.append(text[i:j])
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            comments.append(text[i:j])
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            strings.append(text[i:j + 1])
            out.append('""')
            i = j + 1
        elif c == "'" and not (i and text[i - 1].isalnum()):  # not a digit separator
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append("' '")
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out), comments, strings


def dev_only_files() -> set[str]:
    """The sources the CMake list builds only with the dev tools."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"if\(LSS_DEV_TOOLS\)(.*?)(?:else\(\)|endif\(\))", text, re.S)
    found = set()
    for path in re.findall(r"src/[\w/]+\.cpp", block.group(1) if block else ""):
        found.add(path)
        found.add(path[:-4] + ".h")
    return found


def group_of(rel: str) -> str:
    parts = rel.split("/")
    if parts[0] == "tests":
        return "tests"
    if len(parts) == 2:  # src/mod.cpp, src/modules.cpp
        return "session"
    return GROUPS.get(parts[1], "?")


def scan():
    names: dict[str, dict] = {}
    comment_hits = string_hits = 0
    words = re.compile(r"player 2|Player 2|\bp2\b|\bP2\b|pad 2|Pad 2")
    prose_by_group: dict[str, int] = {}
    string_by_group: dict[str, int] = {}
    dev = dev_only_files()
    for top in ("src", "tests"):
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in (".h", ".cpp") or "ue4ss_compat" in path.parts:
                continue
            rel = path.relative_to(ROOT).as_posix()
            code, comments, strings = strip(path.read_bytes().decode("utf-8", "replace"))
            group = group_of(rel)
            c_hits = sum(len(words.findall(c)) for c in comments)
            s_hits = sum(len(words.findall(s)) for s in strings)
            comment_hits += c_hits
            string_hits += s_hits
            prose_by_group[group] = prose_by_group.get(group, 0) + c_hits
            string_by_group[group] = string_by_group.get(group, 0) + s_hits
            seen = set()  # a name two families match counts once, under the first
            for family, pattern in FAMILIES.items():
                for match in re.finditer(pattern, code):
                    if match.start() in seen:
                        continue
                    seen.add(match.start())
                    name = match.group(0)
                    if (rel, name) in KEEP_IN:
                        continue
                    entry = names.setdefault(name, {"family": family, "files": {}, "sites": 0})
                    entry["sites"] += 1
                    entry["files"][rel] = entry["files"].get(rel, 0) + 1
    for entry in names.values():
        entry["groups"] = sorted({group_of(f) for f in entry["files"]})
        entry["devOnly"] = all(f in dev or f.startswith("tests/") for f in entry["files"]) and any(
            f in dev for f in entry["files"]
        )
    return names, comment_hits, string_hits, prose_by_group, string_by_group


def short(path: str) -> str:
    return path.removeprefix("src/").removeprefix("tests/")


def ledger(names, comment_hits, string_hits, prose, strs) -> str:
    live = {n: e for n, e in names.items() if n not in KEEP}
    shared = {n: e for n, e in live.items() if len([g for g in e["groups"] if g != "tests"]) > 1}
    lines = [
        "# Seats step 7c: the ledger of names that still mean player 2",
        "",
        "Generated by `tools/naming_census.py --ledger`, from code with its comments and strings stripped. Run it again",
        "rather than editing this file. Front page: [seats](seats.md) step 7.",
        "",
        f"**{len(live)} names, {sum(e['sites'] for e in live.values())} sites in code.** Apart from those, comments "
        f"mention player 2 {comment_hits} times and strings {string_hits} times (stage 7d).",
        "",
        "## Names used by more than one group (named once, by the lead)",
        "",
        "| Name | Family | Sites | Groups | Files |",
        "|---|---|---|---|---|",
    ]
    for name, e in sorted(shared.items(), key=lambda kv: -kv[1]["sites"]):
        files = ", ".join(short(f) for f in list(e["files"])[:6]) + (" ..." if len(e["files"]) > 6 else "")
        lines.append(f"| `{name}` | {e['family']} | {e['sites']} | {', '.join(e['groups'])} | {files} |")
    groups = sorted({g for e in live.values() for g in e["groups"]})
    for group in groups:
        rows = [(n, e) for n, e in live.items() if n not in shared and group in e["groups"]]
        lines += ["", f"## Group: {group}", "", f"{sum(e['sites'] for _, e in rows)} sites, {len(rows)} names; "
                  f"comments {prose.get(group, 0)}, strings {strs.get(group, 0)}.", "",
                  "| Name | Family | Sites | Dev only | Files |", "|---|---|---|---|---|"]
        for name, e in sorted(rows, key=lambda kv: (kv[1]["family"], -kv[1]["sites"], kv[0])):
            files = ", ".join(f"{short(f)} ({c})" for f, c in list(e["files"].items())[:4])
            files += " ..." if len(e["files"]) > 4 else ""
            lines.append(f"| `{name}` | {e['family']} | {e['sites']} | {'yes' if e['devOnly'] else ''} | {files} |")
    lines += ["", "## Kept: not a player", ""]
    for name, why in sorted(KEEP.items()):
        if name in names:
            lines.append(f"- `{name}`: {why} ({names[name]['sites']} sites).")
    for (rel, name), why in sorted(KEEP_IN.items()):
        lines.append(f"- `{name}` in {short(rel)}: {why}.")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ledger", type=pathlib.Path)
    parser.add_argument("--left", action="store_true")
    parser.add_argument("--group")
    args = parser.parse_args()
    names, comment_hits, string_hits, prose, strs = scan()
    live = {n: e for n, e in names.items() if n not in KEEP}
    if args.group:
        dev = dev_only_files()
        by_file: dict[str, list[str]] = {}
        for name, e in live.items():
            for f, c in e["files"].items():
                if group_of(f) == args.group and f not in dev:
                    by_file.setdefault(f, []).append(f"{name}:{c}")
        total = 0
        for f in sorted(by_file):
            print(f"{f}: " + " ".join(sorted(by_file[f])))
            total += sum(int(x.rsplit(":", 1)[1]) for x in by_file[f])
        print(f"{total} sites in {len(by_file)} files")
        return 0
    if args.ledger:
        args.ledger.write_text(ledger(names, comment_hits, string_hits, prose, strs), encoding="utf-8", newline="\n")
    if args.left:
        for name in sorted(live):
            print(name)
        return 1 if live else 0
    print(f"{len(live)} names, {sum(e['sites'] for e in live.values())} sites in code; "
          f"comments {comment_hits}, strings {string_hits}")
    for family in FAMILIES:
        rows = [e for e in live.values() if e["family"] == family]
        print(f"  {family}: {len(rows)} names, {sum(e['sites'] for e in rows)} sites")
    sites: dict[str, int] = {}
    for e in live.values():
        for f, c in e["files"].items():
            sites[group_of(f)] = sites.get(group_of(f), 0) + c
    for group, count in sorted(sites.items(), key=lambda kv: -kv[1]):
        nfiles = len({f for e in live.values() for f in e["files"] if group_of(f) == group})
        print(f"  group {group}: {count} sites in {nfiles} files; comments {prose.get(group, 0)}, "
              f"strings {strs.get(group, 0)}")
    shared = [n for n, e in live.items() if len([g for g in e["groups"] if g != "tests"]) > 1]
    print(f"  shared across groups: {len(shared)} names")
    print(f"  dev only: {sum(1 for e in live.values() if e['devOnly'])} names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
