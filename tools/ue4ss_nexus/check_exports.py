"""Checks a built mod DLL against the UE4SS build players have: every symbol the mod imports from UE4SS.dll must be
one that DLL exports. A name match is the necessary check (a layout mismatch stays invisible here), so the mod is
built from that build's source and this is the gate after each build.

    python tools/ue4ss_nexus/check_exports.py <mod main.dll> <UE4SS.dll>

Exit 0 when every import resolves; 1 with the missing names (demangled where undname.exe is found) otherwise.
"""

import glob
import os
import subprocess
import sys

import pefile


def exports(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    return {e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}


def imports_from(path, dll):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        if entry.dll.decode().lower() == dll:
            return {i.name.decode() for i in entry.imports if i.name}
    return set()


def demangle(names):
    found = glob.glob(r"C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\undname.exe")
    if not found or not names:
        return names
    out = subprocess.run([found[0]] + list(names), capture_output=True, text=True).stdout
    lines = [line.split('is :- ', 1)[1].strip(' "') for line in out.splitlines() if 'is :- ' in line]
    return lines or names


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    mod, loader = sys.argv[1], sys.argv[2]
    needed = imports_from(mod, "ue4ss.dll")
    offered = exports(loader)
    missing = sorted(needed - offered)
    print(f"{os.path.basename(mod)}: {len(needed)} imports from UE4SS.dll; {os.path.basename(loader)}: "
          f"{len(offered)} exports; missing {len(missing)}")
    for name in demangle(missing):
        print("  " + name)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
