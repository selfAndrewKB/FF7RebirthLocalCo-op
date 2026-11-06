"""Is the working tree's change to src/ and tests/ a rename and nothing else?

Compares each changed file with its committed version, token by token, with comments set apart: the two token
streams must have the same length, and wherever they differ both tokens must be identifiers. So a changed string, a
changed number, an added or removed statement, or a reordered line shows; whitespace and line breaks do not. Comments
are compared with the file's renames applied to the old text: a comment that differs beyond that was reworded.

    python -I tools/rename_audit.py [REV]          against REV (default HEAD); exit 0 if every file is a pure rename
    python -I tools/rename_audit.py --names        also list each file's renames, old -> new

It does not judge the names, and it cannot see a new name that hides another one: tools/same_code.py shows that.
"""
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOKEN = re.compile(r"""
      (?P<comment>//[^\n]*|/\*.*?\*/)
    | (?P<string>(?:L|u8|u|U)?"(?:[^"\\\n]|\\.)*"|(?:L|u8|u|U)?'(?:[^'\\\n]|\\.)+')
    | (?P<number>\d[\w.']*)
    | (?P<word>[A-Za-z_]\w*)
    | (?P<mark>\S)
""", re.S | re.X)


def tokens(text: str):
    code, comments = [], []
    for match in TOKEN.finditer(text):
        kind = match.lastgroup
        if kind == "comment":
            comments.append(match.group())
        else:
            code.append((kind, match.group()))
    return code, comments


def words_of(comment: str) -> list[str]:
    return re.findall(r"\w+|[^\w\s]", comment)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                          errors="replace").stdout


def main() -> int:
    show_names = "--names" in sys.argv
    revs = [a for a in sys.argv[1:] if not a.startswith("--")]
    rev = revs[0] if revs else "HEAD"
    changed = [p for p in git("diff", "--name-only", rev, "--", "src", "tests").splitlines() if p.endswith((".h", ".cpp"))]
    problems = 0
    total = collections.Counter()
    for rel in changed:
        old_code, old_comments = tokens(git("show", f"{rev}:{rel}"))
        new_code, new_comments = tokens((ROOT / rel).read_bytes().decode("utf-8", "replace"))
        renames = collections.Counter()
        if len(old_code) != len(new_code):
            problems += 1
            first = next((i for i, (a, b) in enumerate(zip(old_code, new_code)) if a != b and not (a[0] == b[0] == "word")),
                         min(len(old_code), len(new_code)))
            print(f"NOT A RENAME {rel}: {len(old_code)} tokens became {len(new_code)}; first break near "
                  f"{' '.join(t for _, t in old_code[max(0, first - 6):first + 6])!r}")
            continue
        for (old_kind, old), (new_kind, new) in zip(old_code, new_code):
            if old == new:
                continue
            if old_kind == new_kind == "word":
                renames[(old, new)] += 1
            else:
                problems += 1
                print(f"NOT A RENAME {rel}: {old_kind} {old[:60]!r} became {new_kind} {new[:60]!r}")
        mapping = collections.defaultdict(set)
        for (old, new) in renames:
            mapping[old].add(new)
        # Comments: the old ones with the file's renames applied, as bags of words, against the new ones.
        def bag(comments, apply):
            words = collections.Counter()
            for comment in comments:
                for word in words_of(comment):
                    if apply and word in mapping and len(mapping[word]) == 1:
                        word = next(iter(mapping[word]))
                    words[word] += 1
            return words
        before, after = bag(old_comments, True), bag(new_comments, False)
        gone, came = before - after, after - before
        gone.pop("/", None), came.pop("/", None)  # a comment wrapped onto one more line gains a "//"
        if gone or came:
            print(f"COMMENT WORDS {rel}: gone {dict(gone)}; new {dict(came)}")
        total.update(renames)
        if show_names:
            print(f"{rel}: " + ", ".join(f"{o}->{n} x{c}" for (o, n), c in sorted(renames.items())))
    print(f"{len(changed)} files, {sum(total.values())} renamed sites, {len({o for o, _ in total})} old names; "
          f"{problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
