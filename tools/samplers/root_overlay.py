"""Read-only sampler: what Square's HUD overlay (SEndRootOverlay) lays itself out against.

    python tools/samplers/root_overlay.py              one reading per second, 10 readings
    python tools/samplers/root_overlay.py -n 30 -i 0.2

The HUD is one SEndRootOverlay singleton rendered into one UI image per frame
(Rebirth/Documentation/research/RE-p2-hud.md). Each frame it caches player 0's view rectangle, the
UI size and a host scale, and the marker conversion (EndUI__ProjectWorldToRootOverlay) uses them:

    UI = (Screen - ViewMin - ViewSize/2) * UIScale / HostScale + UISize/2

This prints those cached fields. It only reads memory (ReadProcessMemory) and never pauses the game.
A reading is kept only if the overlay's frame stamp (+0x448, written from GFrameNumber at the start of
each layout) is the same before and after the fields are read, so no row mixes two frames.

Addresses are IDA addresses of ff7rebirth_.exe (image base 0x140000000), rebased on the running module.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

PROCESS_NAME = "ff7rebirth_.exe"
IDA_BASE = 0x140000000
ROOT_OVERLAY_SINGLETON = 0x1491D1948  # SEndRootOverlay* (SEndRootOverlay__GetOrCreate 0x1410EAB8C)

FIELDS = (  # (name, offset, format) in the SEndRootOverlay object
    ("view.x", 0x318, "f"),   # player 0's view rect Min (written at 0x140C95F2C)
    ("view.y", 0x31C, "f"),
    ("ui.w", 0x320, "f"),     # UI size used by the marker conversion
    ("ui.h", 0x324, "f"),
    ("view.w", 0x328, "f"),   # player 0's view rect size (0x140C95F3C)
    ("view.h", 0x32C, "f"),
    ("host", 0x330, "f"),     # host scale (viewport host geometry)
)
FRAME_STAMP = 0x448

# Globals the composite layer is sized from (RE-p2-hud.md, pass 2), as (name, IDA address, format).
GLOBALS = (
    ("cvar.w", 0x149318B18, "i"),     # r.CompositeLayer.FixedWidth, render-thread copy
    ("cvar.h", 0x149318B1C, "i"),     # r.CompositeLayer.FixedHeight, render-thread copy
    ("res.x", 0x14705CD00, "I"),      # GSystemResolution.ResX
    ("res.y", 0x14705CD04, "I"),      # GSystemResolution.ResY
    ("alloc.w", 0x1490F82E8, "i"),    # the composite target's last allocation size
    ("alloc.h", 0x1490F82EC, "i"),
)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
LIST_MODULES_ALL = 0x03

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_size_t)]


def find_process() -> int | None:
    ids = (wt.DWORD * 4096)()
    needed = wt.DWORD()
    psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(needed))
    for pid in ids[: needed.value // ctypes.sizeof(wt.DWORD)]:
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            continue
        name = ctypes.create_unicode_buffer(260)
        size = wt.DWORD(len(name))
        ok = kernel32.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size))
        kernel32.CloseHandle(handle)
        if ok and name.value.lower().endswith("\\" + PROCESS_NAME):
            return pid
    return None


def module_base(handle: int) -> int:
    modules = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed),
                                      LIST_MODULES_ALL):
        raise OSError(ctypes.get_last_error(), "EnumProcessModulesEx failed")
    name = ctypes.create_unicode_buffer(260)
    for module in modules[: needed.value // ctypes.sizeof(wt.HMODULE)]:
        psapi.GetModuleBaseNameW(handle, wt.HMODULE(module), name, len(name))
        if name.value.lower() == PROCESS_NAME:
            return module
    raise LookupError(f"{PROCESS_NAME} is not among the process's modules")


def read(handle: int, address: int, size: int) -> bytes:
    buffer = ctypes.create_string_buffer(size)
    done = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buffer, size, ctypes.byref(done)) \
            or done.value != size:
        raise OSError(ctypes.get_last_error(), f"cannot read {size} bytes at {address:#x}")
    return buffer.raw


def sample(handle: int, overlay: int) -> tuple[int, dict[str, float]] | None:
    """One consistent reading, or None if a frame boundary fell inside it."""
    before = struct.unpack("<I", read(handle, overlay + FRAME_STAMP, 4))[0]
    block = read(handle, overlay + 0x318, 0x20)
    after = struct.unpack("<I", read(handle, overlay + FRAME_STAMP, 4))[0]
    if before != after:
        return None
    values = {name: struct.unpack_from("<" + fmt, block, offset - 0x318)[0] for name, offset, fmt in FIELDS}
    return before, values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-n", type=int, default=10, help="readings to print")
    parser.add_argument("-i", type=float, default=1.0, help="seconds between readings")
    args = parser.parse_args()

    pid = find_process()
    if pid is None:
        print(f"{PROCESS_NAME} is not running")
        return 1
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        print(f"cannot open process {pid} for reading (error {ctypes.get_last_error()})")
        return 1
    try:
        base = module_base(handle)
        singleton = base + (ROOT_OVERLAY_SINGLETON - IDA_BASE)
        overlay = struct.unpack("<Q", read(handle, singleton, 8))[0]
        vtable = struct.unpack("<Q", read(handle, overlay, 8))[0] if overlay else 0
        print(f"pid {pid}, module {base:#x}, overlay {overlay:#x} (vtable {vtable:#x}, "
              f"IDA {vtable - base + IDA_BASE:#x})")
        if not overlay:
            print("no SEndRootOverlay yet")
            return 1

        values = []
        for name, address, fmt in GLOBALS:
            raw = read(handle, base + (address - IDA_BASE), 4)
            values.append(f"{name} {struct.unpack('<' + fmt, raw)[0]}")
        print("globals: " + ", ".join(values))

        print("frame     " + " ".join(f"{name:>8}" for name, _, _ in FIELDS))
        shown, rejected = 0, 0
        while shown < args.n:
            reading = sample(handle, overlay)
            if reading is None:
                rejected += 1
                continue
            frame, values = reading
            print(f"{frame:<9} " + " ".join(f"{values[name]:8.3f}" for name, _, _ in FIELDS))
            shown += 1
            time.sleep(args.i)
        if rejected:
            print(f"({rejected} reading(s) rejected: a frame boundary fell inside them)")
        return 0
    finally:
        kernel32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())
