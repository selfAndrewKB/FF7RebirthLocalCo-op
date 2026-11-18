"""Makes the release zip, to unpack into the game's End\\Binaries\\Win64 folder over the Nexus Mods UE4SS.

    python package.py              build (the host tests, then the players' build), then zip into dist/
    python package.py --no-build   zip what is built (refused when the built DLL or app is not this source's version;
                                   a change to the source without a new version is not seen: build)
    python package.py --nexus      the same without the setup app (Nexus Mods holds uploads with programs in them for a
                                   manual check; the app is then fetched from the repository's releases)

The zip holds ue4ss/Mods/LocalSplitScreen/ with dlls/main.dll and the setup app from the players' build (build-release/,
configured with -DLSS_DEV_TOOLS=OFF: no console, no dev commands; README.md, Building), enabled.txt (so UE4SS starts the
mod without a line in mods.txt), the licence with its exception, and UE4SS's licence for the files below; and beside it,
in ue4ss/, the loader's UE4SS-settings.ini and Mods/mods.txt as its Nexus zip ships them (tools/ue4ss_nexus/config/),
one line changed in each: GuiConsoleEnabled = 0 (UE4SS's own window, drawn with OpenGL, crashed the game at start) and
SplitScreenMod : 0 (the loader turns UE4SS's example Lua split-screen mod on, which switches the engine's own split
screen on at start). The version is src/mod.cpp's and the setup app's name src/views/display_move.h's, the lines the
build reads too; the zip is named by the version's number alone (LocalSplitScreen-v1.1.0.zip for "1.1.0-name"). No
settings file of the mod is packed: the mod writes it.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
CONFIG = "Game__Shipping__Win64"
FOLDER = "LocalSplitScreen"
RELEASE = ROOT / "build-release"
LOADER = ROOT / "tools" / "ue4ss_nexus" / "config"  # the Nexus loader's own files, as its zip ships them
# The loader's files the zip carries, each with the one line the mod needs changed.
LOADER_FILES = (
    ("ue4ss/UE4SS-settings.ini", "UE4SS-settings.ini", rb"(?m)^GuiConsoleEnabled\s*=\s*1[ \t]*(?=\r?$)", b"GuiConsoleEnabled = 0"),
    ("ue4ss/Mods/mods.txt", "mods.txt", rb"(?m)^SplitScreenMod\s*:\s*1[ \t]*(?=\r?$)", b"SplitScreenMod : 0"),
)


def line_value(path: pathlib.Path, pattern: str) -> str:
    found = re.search(pattern, path.read_text(encoding="utf-8"))
    if not found:
        sys.exit(f"{path.relative_to(ROOT)}: no line matches {pattern}")
    return found.group(1)


def number(version: str) -> str:
    """The version's number, its name dropped ("1.1.0" for "1.1.0-name"): what the zips and the release tag carry."""
    return version.split("-")[0]


def zip_path(version: str, nexus: bool) -> pathlib.Path:
    return ROOT / "dist" / f"{FOLDER}-v{number(version)}{'-nexus' if nexus else ''}.zip"


def run(*command: str) -> None:
    print(">", " ".join(command))
    if subprocess.run(command, cwd=ROOT).returncode != 0:
        sys.exit(f"failed: {' '.join(command)}")


def release_without_dev_tools() -> bool:
    cache = RELEASE / "CMakeCache.txt"
    return cache.exists() and re.search(r"^LSS_DEV_TOOLS:BOOL=OFF$", cache.read_text(encoding="utf-8"), re.M) is not None


def loader_config() -> list[tuple[str, bytes]]:
    """The loader's settings and mod list as files under ue4ss/, the GUI console and its Lua split-screen mod turned off."""
    files = []
    for name, source, pattern, line in LOADER_FILES:
        path = LOADER / source
        if not path.exists():
            sys.exit(f"no {path.relative_to(ROOT)}: see tools/ue4ss_nexus/README.md")
        data, count = re.subn(pattern, line, path.read_bytes())
        if count != 1:
            sys.exit(f"{path.relative_to(ROOT)}: expected one line to become '{line.decode()}', found {count}")
        files.append((name, data))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--no-build", action="store_true", help="zip what is already built")
    parser.add_argument("--nexus", action="store_true", help="leave the setup app out")
    args = parser.parse_args()

    version = line_value(ROOT / "src" / "mod.cpp", r'kVersion = L"([^"]+)"')
    setup = line_value(ROOT / "src" / "views" / "display_move.h", r'kSetupExe = L"([^"]+)"')
    if not release_without_dev_tools():
        sys.exit("build-release/ is not configured with -DLSS_DEV_TOOLS=OFF: see README.md, Building")
    if not args.no_build:
        # The host tests on the development build (under the build lock), then the players' build.
        # PowerShell 7 (pwsh): under Windows PowerShell 5.1 the script's cmake stderr lines become terminating errors.
        run("pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "tools" / "build_locked.ps1"),
            "-Tests")
        run("cmake", "--build", str(RELEASE), "--config", CONFIG, "--target", "LocalSplitScreen", "SetupApp")

    built = RELEASE / CONFIG
    dll = built / "LocalSplitScreen.dll"
    app = built / setup
    packed = [dll] if args.nexus else [dll, app]
    for path in packed:
        if not path.exists():
            sys.exit(f"not built: {path.relative_to(ROOT)}")
    # Both carry the version as wide text, the DLL in its code and the app in its file properties: a build of other source
    # is not packed under this one's name. The text is matched with its end, so 1.2.3 does not pass for 1.2.30.
    wide = (version + "\0").encode("utf-16-le")
    for path in packed:
        if wide not in path.read_bytes():
            sys.exit(f"{path.relative_to(ROOT)} is not version {version}: build first")

    mod = f"ue4ss/Mods/{FOLDER}"
    files = [(f"{mod}/dlls/main.dll", dll.read_bytes())]
    if not args.nexus:
        files.append((f"{mod}/{setup}", app.read_bytes()))
    files.append((f"{mod}/enabled.txt", b""))
    for name in ("LICENSE", "LICENSE-EXCEPTION"):
        if not (ROOT / name).exists():
            sys.exit(f"no {name} in the repository")
        files.append((f"{mod}/{name}", (ROOT / name).read_bytes()))
    if not (LOADER / "LICENSE").exists():
        sys.exit(f"no {(LOADER / 'LICENSE').relative_to(ROOT)}: see tools/ue4ss_nexus/README.md")
    files.append((f"{mod}/LICENSE-UE4SS", (LOADER / "LICENSE").read_bytes()))
    files += loader_config()

    out = zip_path(version, args.nexus)
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in files:
            archive.writestr(name, data)
            print(f"  {name:<58} {len(data):>9} bytes  {hashlib.sha256(data).hexdigest()[:8].upper()}")
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
