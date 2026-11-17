"""One game exe read from disk: sections, function ranges from .pdata, and masked disassembly.

Masking: the fields that legitimately change when code moves between builds are blanked, so the
rest of the bytes can be compared exactly. Those fields are rel32 branch displacements and
RIP-relative disp32 operands (both are distances to other code or data, which shift when anything
between them grows or shrinks). Everything else, including struct offsets and immediates, must be
identical for two pieces of code to count as the same.
"""
from __future__ import annotations

import bisect
import re
import struct
from dataclasses import dataclass

import capstone
import pefile
from capstone import x86_const

IDA_BASE = 0x140000000


def build_exe(version: str) -> str:
    """The kept copy of a game build's exe: Rebirth/builds/<version>/ff7rebirth_.exe beside the mod's repository, or
    under the folder LSS_BUILDS_DIR names."""
    import os
    import pathlib
    builds = pathlib.Path(os.environ.get("LSS_BUILDS_DIR") or pathlib.Path(__file__).resolve().parents[3] / "builds")
    return str(builds / version / "ff7rebirth_.exe")


@dataclass
class Insn:
    address: int        # IDA address
    size: int
    masked: tuple       # (offset, size) pairs within the instruction that are blanked
    targets: tuple      # (field offset, target IDA address) for each masked field
    loose: tuple = ()   # every displacement and immediate field (offset, size): for structural matching


class Image:
    def __init__(self, path: str):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.data = open(path, "rb").read()
        self.sections = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            self.sections.append((name, IDA_BASE + s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData))
        self.text = next(s for s in self.sections if s[0] == ".text")
        # Function chunks from the exception directory (RUNTIME_FUNCTION begin/end).
        funcs = []
        for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []) or []:
            funcs.append((IDA_BASE + e.struct.BeginAddress, IDA_BASE + e.struct.EndAddress))
        funcs.sort()
        self.func_begin = [f[0] for f in funcs]
        self.func_end = [f[1] for f in funcs]
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.detail = True
        self._insn_cache: dict[int, list[Insn]] = {}

    # --- addresses -------------------------------------------------------------------------
    def section_of(self, va: int):
        for name, start, vsize, _, _ in self.sections:
            if start <= va < start + max(vsize, 1):
                return name
        return None

    def off(self, va: int):
        for _, start, vsize, raw, rawsize in self.sections:
            if start <= va < start + vsize:
                o = va - start
                return raw + o if o < rawsize else None
        return None

    def va(self, off: int):
        for _, start, vsize, raw, rawsize in self.sections:
            if raw <= off < raw + rawsize:
                return start + (off - raw)
        return None

    def read(self, va: int, n: int) -> bytes:
        o = self.off(va)
        if o is None:
            return b""
        return self.data[o:o + n]

    def u64(self, va: int):
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if len(b) == 8 else None

    def function_at(self, va: int):
        """The .pdata chunk holding va, as (begin, end), or None (leaf function or not code)."""
        i = bisect.bisect_right(self.func_begin, va) - 1
        if i >= 0 and self.func_begin[i] <= va < self.func_end[i]:
            return self.func_begin[i], self.func_end[i]
        return None

    def next_function_after(self, va: int):
        i = bisect.bisect_right(self.func_begin, va)
        return self.func_begin[i] if i < len(self.func_begin) else None

    # --- disassembly -------------------------------------------------------------------------
    def disasm(self, start: int, end: int) -> list[Insn]:
        """Linear disassembly of [start, end), with masks. Stops early at undecodable bytes."""
        key = (start << 20) ^ end
        if key in self._insn_cache:
            return self._insn_cache[key]
        code = self.read(start, end - start)
        out = []
        for ins in self.cs.disasm(code, start):
            masked, targets = [], []
            # RIP-relative memory operand: disp32 at disp_offset.
            for op in ins.operands:
                if op.type == x86_const.X86_OP_MEM and op.mem.base == x86_const.X86_REG_RIP and ins.disp_size == 4:
                    masked.append((ins.disp_offset, 4))
                    targets.append((ins.disp_offset, ins.address + ins.size + op.mem.disp))
            # rel32 branches (call/jmp/jcc): the immediate is the displacement.
            if ins.group(x86_const.X86_GRP_JUMP) or ins.group(x86_const.X86_GRP_CALL):
                if ins.imm_size == 4 and ins.operands and ins.operands[0].type == x86_const.X86_OP_IMM:
                    masked.append((ins.imm_offset, 4))
                    targets.append((ins.imm_offset, ins.operands[0].imm))
            # mov r64, imm64 holding an address inside the image (relocated absolute pointer).
            if ins.imm_size == 8:
                imm = ins.operands[-1].imm if ins.operands and ins.operands[-1].type == x86_const.X86_OP_IMM else 0
                if IDA_BASE <= imm < IDA_BASE + 0x10000000:
                    masked.append((ins.imm_offset, 8))
                    targets.append((ins.imm_offset, imm))
            loose = list(masked)
            if ins.disp_size and (ins.disp_offset, ins.disp_size) not in loose:
                loose.append((ins.disp_offset, ins.disp_size))
            if ins.imm_size and (ins.imm_offset, ins.imm_size) not in loose:
                loose.append((ins.imm_offset, ins.imm_size))
            out.append(Insn(ins.address, ins.size, tuple(masked), tuple(targets), tuple(loose)))
        if len(self._insn_cache) > 4000:
            self._insn_cache.clear()
        self._insn_cache[key] = out
        return out

    def masked_window(self, insns: list[Insn], start: int, end: int, loose: bool = False):
        """(bytes, mask) for [start, end) using the instructions covering it; mask[i] True = blanked.
        loose: also blank every displacement and immediate (structure only)."""
        raw = bytearray(self.read(start, end - start))
        mask = [False] * len(raw)
        for ins in insns:
            if ins.address + ins.size <= start or ins.address >= end:
                continue
            for o, n in (ins.loose if loose else ins.masked):
                for k in range(n):
                    p = ins.address + o + k - start
                    if 0 <= p < len(raw):
                        mask[p] = True
        return bytes(raw), mask

    # --- search -------------------------------------------------------------------------------
    def search(self, pattern: bytes, mask: list, limit: int = 4, lo: int | None = None, hi: int | None = None):
        """IDA addresses in .text where pattern matches with masked bytes ignored."""
        parts = []
        for b, m in zip(pattern, mask):
            parts.append(b"." if m else re.escape(bytes([b])))
        rx = re.compile(b"".join(parts), re.DOTALL)
        _, tstart, tsize, traw, trawsize = self.text
        s = traw if lo is None else max(traw, self.off(lo) or traw)
        e = traw + trawsize if hi is None else min(traw + trawsize, self.off(hi) or traw + trawsize)
        hits = []
        for m in rx.finditer(self.data, s, e):
            hits.append(self.va(m.start()))
            if len(hits) >= limit:
                break
        return hits
