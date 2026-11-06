"""Do two builds of the DLL hold the same code, whatever order the linker laid it out in?

A rename changes no behaviour, but it can change the order of a file's functions, so the raw bytes of .text differ.
This disassembles both DLLs (MSVC's dumpbin), splits the code into functions by the function table, and replaces every
address by what it points at: an offset inside the function, the called function (following the incremental linker's
jump thunks), a string's text, or a global. Two builds with the same functions, counted with their repeats, hold the
same code. A rename that picked up the wrong variable changes a stack offset, a register or the global and shows as a
differing function.

A global is named by its rank: its place in the order of every data address the code refers to. Its distance from the
start of .data would not do, because a rename moves the globals. The name of a lambda's type holds the name of the
function around it, and where the lambda is kept in a std::function that name is stored as text among the globals, so
a longer or shorter function name shifts everything after it. The order stays, so the ranks stay; a function that
reads another global gets another rank. If a build ever changed the order, every function would read as different:
the comparison would fail, not pass wrongly.

    python -I tools/same_code.py A.dll B.dll       exit 0 if the code is the same; else lists what differs
    python -I tools/same_code.py A.dll B.dll --globals-by-offset
                                                   stricter: a global is its distance from the start of its section
    python -I tools/same_code.py A.dll B.dll --ignore-data-layout
                                                   a diagnosis: a global is its section alone
"""
import collections
import hashlib
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

ADDRESS = re.compile(r"\b([0-9A-F]{16})h?\b")
LINE = re.compile(r"^\s+([0-9A-F]{16}): (\S+)\s*(.*?)\s*$")
TOOLS = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC")


def dumpbin() -> str:
    return str(sorted(TOOLS.iterdir())[-1] / "bin" / "Hostx64" / "x64" / "dumpbin.exe")


class Image:
    def __init__(self, path: pathlib.Path):
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        count = struct.unpack_from("<H", self.data, pe + 6)[0]
        optional = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<Q", self.data, pe + 24 + 24)[0]
        self.sections = []
        for i in range(count):
            at = pe + 24 + optional + 40 * i
            name = self.data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
            virtual, rva, raw_size, raw_at = struct.unpack_from("<IIII", self.data, at + 8)
            self.sections.append((name, rva, virtual, raw_at, raw_size))
        self.functions = []
        for name, rva, virtual, raw_at, raw_size in self.sections:
            if name == ".pdata":
                body = self.data[raw_at:raw_at + min(virtual, raw_size)]
                for i in range(0, len(body) - 11, 12):
                    begin, end, _ = struct.unpack_from("<III", body, i)
                    if begin:
                        self.functions.append((begin, end))
        self.functions.sort()

    def section_of(self, rva: int):
        for section in self.sections:
            if section[1] <= rva < section[1] + section[2]:
                return section
        return None

    def string_at(self, rva: int) -> str | None:
        """The text at a data address, if it reads as a narrow or wide string."""
        section = self.section_of(rva)
        if section is None:
            return None
        at = section[3] + rva - section[1]
        if at >= section[3] + section[4]:
            return None
        raw = self.data[at:at + 96]
        wide = raw[1::2]
        for text, step in ((raw[0::2], 2), (raw, 1)):
            if step == 2 and any(wide[:4]):
                continue
            end = text.find(b"\0")
            piece = text[:end] if end >= 0 else text
            if len(piece) >= 4 and all(32 <= c < 127 or c in (9, 10, 13) for c in piece):
                return piece.decode("ascii")
        return None


def blocks(image: Image, path: pathlib.Path):
    """The code as blocks of (rva, mnemonic, operands): one per function, and, outside the function table, one per
    run that ends in a jump or a return (the thunks and the leaf functions)."""
    # A copy with its debug link blanked: dumpbin would print symbol names for a build whose PDB it finds, addresses
    # for one whose PDB is gone.
    with tempfile.TemporaryDirectory() as folder:
        copy = pathlib.Path(folder) / "code.dll"
        copy.write_bytes(image.data.replace(b"RSDS", b"XXXX"))
        text = subprocess.run([dumpbin(), "/nologo", "/disasm:nobytes", str(copy)], capture_output=True, text=True,
                              errors="replace").stdout
    starts = [f[0] for f in image.functions]
    ends = dict(image.functions)
    out, current, current_end = [], [], None
    position = 0
    for match in map(LINE.match, text.splitlines()):
        if match is None:
            continue
        rva = int(match.group(1), 16) - image.base
        mnemonic, operands = match.group(2), match.group(3)
        if mnemonic == "int" and operands == "3":
            continue
        while position < len(starts) and starts[position] < rva:
            position += 1
        if position < len(starts) and starts[position] == rva:
            if current:
                out.append(current)
            current, current_end = [], ends[rva]
        elif current_end is not None and rva >= current_end:
            if current:
                out.append(current)
            current, current_end = [], None
        current.append((rva, mnemonic, operands))
        if current_end is None and mnemonic in ("jmp", "ret"):
            out.append(current)
            current = []
    if current:
        out.append(current)
    return out


def fingerprints(path: pathlib.Path, globals_by: str = "rank") -> tuple[collections.Counter, dict]:
    """The count of each block's hash, and one block per hash. `globals_by` says how a place in .data or .tls is named:
    "rank" (its place in the order of all the places the code refers to), "offset" (its distance from the section's
    start) or "section" (the section alone: a function that reads the wrong global no longer shows)."""
    image = Image(path)
    code = blocks(image, path)
    start_of = {block[0][0]: i for i, block in enumerate(code)}
    thunk = {b[0][0]: int(ADDRESS.search(b[0][2]).group(1), 16) - image.base for b in code
             if len(b) == 1 and b[0][1] == "jmp" and ADDRESS.search(b[0][2])}

    def resolve(rva: int) -> int:
        for _ in range(8):
            if rva not in thunk:
                break
            rva = thunk[rva]
        return rva

    referred = set()
    for block in code:
        for _, _, operands in block:
            for match in ADDRESS.finditer(operands):
                target = resolve(int(match.group(1), 16) - image.base)
                section = image.section_of(target)
                if section is not None and section[0] in (".data", ".tls"):
                    referred.add(target)
    rank = {target: place for place, target in enumerate(sorted(referred))}

    def round_of(previous):
        hashes = []
        for index, block in enumerate(code):
            first = block[0][0]
            last = block[-1][0]
            digest = hashlib.sha1()
            for _, mnemonic, operands in block:
                def replace(match):
                    rva = int(match.group(1), 16) - image.base
                    if first <= rva <= last:
                        return f"L{rva - first:x}"
                    target = resolve(rva)
                    if target in start_of:
                        return f"F{previous[start_of[target]]}" if previous else "F"
                    section = image.section_of(target)
                    if section is None:
                        return "?"
                    if section[0] == ".text":
                        return "T"  # inside another function
                    text = image.string_at(target)
                    if text is not None:
                        return f"S{text}"
                    if section[0] in (".data", ".tls"):
                        if globals_by == "rank":
                            return f"{section[0]}#{rank[target]}"
                        if globals_by == "offset":
                            return f"{section[0]}+{target - section[1]:x}"
                    return section[0]
                digest.update(f"{mnemonic} {ADDRESS.sub(replace, operands)}\n".encode())
            hashes.append(digest.hexdigest()[:16])
        return hashes

    hashes = round_of(None)
    for _ in range(2):  # a function's hash takes in the code it calls, two calls deep
        hashes = round_of(hashes)
    counts = collections.Counter(hashes)
    where = {}
    for block, value in zip(code, hashes):
        where.setdefault(value, block)
    return counts, where


def main() -> int:
    globals_by = ("section" if "--ignore-data-layout" in sys.argv
                  else "offset" if "--globals-by-offset" in sys.argv else "rank")
    a_path, b_path = (pathlib.Path(arg) for arg in sys.argv[1:] if not arg.startswith("--"))
    a, a_where = fingerprints(a_path, globals_by)
    b, b_where = fingerprints(b_path, globals_by)
    if globals_by == "section":
        print("globals compared by section only: this run cannot show a function that reads the wrong global")
    only_a = a - b
    only_b = b - a
    print(f"{a_path.name}: {sum(a.values())} blocks; {b_path.name}: {sum(b.values())} blocks")
    if not only_a and not only_b:
        print("the same code")
        return 0
    print(f"DIFFERENT: {sum(only_a.values())} block(s) only in the first, {sum(only_b.values())} only in the second")
    base_a = Image(a_path).base
    base_b = Image(b_path).base
    for label, only, where, base in (("first", only_a, a_where, base_a), ("second", only_b, b_where, base_b)):
        for value in list(only)[:12]:
            block = where[value]
            print(f"  {label}: 0x{block[0][0] + base:X}, {len(block)} instructions; starts "
                  f"{block[0][1]} {block[0][2]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
