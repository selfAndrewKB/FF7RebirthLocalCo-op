"""One declaration per engine address, one style for every checked one (the facts rule, cleanup phase 3).

Walks the mod's sources and fails on an IDA address literal (0x14XXXXXXX) that appears in two files, on a PerBuild
pair whose two values are the same (a pair exists only where a build moved the value), and on a BytesMatch call outside
platform/ (an address with bytes is a FunctionFact, found through FindFunction, so a changed function is logged by
name). Comments and string literals are skipped; the build translation (platform/build_translation.cpp) lists addresses
by design and is skipped, as is the image base. A shared fact belongs in src/facts/, declared once; a fact one module
reads stays in that module.

Usage: facts_once.py <src dir>. Exit 1 with a listing when anything is found; silent and 0 otherwise. Run by ctest.
"""
import os
import re
import sys
from collections import defaultdict

ADDRESS = re.compile(r"\b0x14[0-9A-Fa-f]{7}\b")
DECL = re.compile(r"\b(?:constexpr|const)\b[^;{=]*?\bk\w+\s*(?:=\s*|\{\s*(?:L\s*,\s*)?)\s*(0x14[0-9A-Fa-f]{7})\b")
PER_BUILD = re.compile(r"PerBuild<[^>]+>\s+(\w+)\s*(?:=\s*)?\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\}")
BYTES_MATCH = re.compile(r"\.BytesMatch\s*\(")
SKIP = {os.path.join("platform", "build_translation.cpp")}
PLATFORM = "platform" + os.sep
IMAGE_BASE = "0X140000000"


def strip(text):
    """Blanks comments and string literals, keeping the line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main(root):
    declared = defaultdict(list)  # address -> [(file, line)]
    mentioned = defaultdict(set)  # address -> {file}
    same = []
    raw = []  # (file, line): a BytesMatch call in a module
    for dirpath, _, files in os.walk(root):
        for name in files:
            if not name.endswith((".h", ".cpp")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root)
            if rel in SKIP:
                continue
            code = strip(open(path, encoding="utf-8").read())
            for ln, line in enumerate(code.split("\n"), 1):
                for m in DECL.finditer(line):
                    declared[m.group(1).upper()].append((rel, ln))
                for m in ADDRESS.finditer(line):
                    mentioned[m.group(0).upper()].add(rel)
                for m in PER_BUILD.finditer(line):
                    if int(m.group(2), 0) == int(m.group(3), 0):
                        same.append((rel, ln, m.group(1)))
                if BYTES_MATCH.search(line) and not rel.startswith(PLATFORM):
                    raw.append((rel, ln))
    bad = 0
    for address, files in sorted(mentioned.items()):
        if len(files) > 1 and address != IMAGE_BASE:
            bad += 1
            sites = declared.get(address, [])
            where = ", ".join(f"{f}:{l}" for f, l in sorted(sites)) if sites else ", ".join(sorted(files))
            print(f"{address} in {len(files)} files ({len(sites)} declared): {where}")
    for rel, ln, name in same:
        bad += 1
        print(f"{rel}:{ln}: {name} has the same value in both builds")
    for rel, ln in raw:
        bad += 1
        print(f"{rel}:{ln}: a BytesMatch call outside platform/ (declare a FunctionFact and use FindFunction)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
