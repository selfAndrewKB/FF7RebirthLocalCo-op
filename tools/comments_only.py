"""Checks that a change touched comments and nothing else.

    python -I tools/comments_only.py [REV]        (REV: the commit to compare the working tree with, HEAD by default)

For every .h and .cpp that differs from REV under src/ and tests/, both versions are stripped of their comments and
compared with runs of white space folded to one space. Strings and character literals are kept as they are, so a
reworded log text is reported. A file that differs in anything but comments is listed with the first place it
differs; the exit code is 1 if there is one.
"""
import pathlib
import re
import subprocess
import sys


def code_only(text: str) -> str:
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return re.sub(r"\s+", " ", "".join(out)).strip()


def main() -> int:
    rev = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    names = subprocess.run(
        ["git", "diff", "--name-only", rev, "--", "src", "tests"], capture_output=True, text=True, check=True
    ).stdout.split()
    bad = 0
    checked = 0
    for name in names:
        if not name.endswith((".h", ".cpp")):
            continue
        path = pathlib.Path(name)
        if not path.exists():
            print(f"REMOVED {name}")
            bad += 1
            continue
        old = subprocess.run(["git", "show", f"{rev}:{name}"], capture_output=True)
        if old.returncode != 0:
            print(f"NEW FILE {name}")
            bad += 1
            continue
        before = code_only(old.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n"))
        after = code_only(path.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n"))
        checked += 1
        if before != after:
            at = next((k for k, (a, b) in enumerate(zip(before, after)) if a != b), min(len(before), len(after)))
            print(f"CODE CHANGED {name}: near ...{before[max(0, at - 60):at + 60]}... now ...{after[max(0, at - 60):at + 60]}...")
            bad += 1
    print(f"{checked} changed file(s) checked; {bad} with a change outside comments")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
