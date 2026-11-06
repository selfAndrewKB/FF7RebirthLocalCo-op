"""Hashes of a DLL's sections, and a comparison of two DLLs: does a rename leave the machine code as it was?

A pure rename leaves .text unchanged, so identical code is proof of no behaviour change that needs no game run. .rdata
holds the strings, RTTI's type names and debug directory, so it differs when a class is renamed or the build is newer.

    python -I tools/dll_sections.py A.dll             each section's size and hash
    python -I tools/dll_sections.py A.dll B.dll       the same for both; exit 0 if .text is identical
"""
import hashlib
import pathlib
import struct
import sys


def sections(path: pathlib.Path) -> dict[str, bytes]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional
    found = {}
    for i in range(count):
        at = table + 40 * i
        name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual, _, raw_size, raw_at = struct.unpack_from("<IIII", data, at + 8)
        found[name] = data[raw_at:raw_at + min(virtual, raw_size)]
    return found


def main() -> int:
    paths = [pathlib.Path(a) for a in sys.argv[1:]]
    all_sections = [sections(p) for p in paths]
    for path, found in zip(paths, all_sections):
        print(path)
        for name, body in found.items():
            print(f"  {name:8} {len(body):>10}  {hashlib.sha256(body).hexdigest()[:16]}")
    if len(paths) != 2:
        return 0
    a, b = all_sections
    same = a.get(".text") == b.get(".text")
    print(".text identical" if same else ".text DIFFERS")
    if not same and ".text" in a and ".text" in b:
        x, y = a[".text"], b[".text"]
        diffs = sum(1 for p, q in zip(x, y) if p != q) + abs(len(x) - len(y))
        print(f"  {diffs} bytes differ of {max(len(x), len(y))}; sizes {len(x)} and {len(y)}")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
