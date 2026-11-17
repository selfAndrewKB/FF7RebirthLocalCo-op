"""Offline matching tool: translates every game address the mod uses from one build to another.

Usage:  python match.py <source exe> <new exe> <mod src dir> <out dir> [--version-name 1.006] [--source-name 1.005]

1. Harvest every IDA address (0x14xxxxxxx) written in the mod's code (comments are skipped). They are the source
   build's (the exe the code is written for); "old" below is that build, "new" the one matched.
2. Code addresses: match the whole function holding the address (from .pdata) by its bytes with the
   moving fields blanked (rel32 branches, RIP-relative disp32, absolute image pointers). The match
   is a *range*: old start, new start and the length over which every unblanked byte is identical.
   If the function changed before the address, match a window around the address instead.
3. Data addresses: take the new address from the matched code that references them (the blanked
   disp32 fields of matched ranges give old target -> new target pairs), else from code found
   referencing them by a scan, else, for slots holding code pointers (vtables), by finding the
   translated pointer in the new image.
4. Write report.md, matches.json and the C++ table header.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import re
import sys
from dataclasses import dataclass, field

import numpy as np

from pe_image import IDA_BASE, Image

ADDR_RX = re.compile(r"\b0x(14[0-9A-Fa-f]{7})\b")
SUM_RX = re.compile(r"\b0x(14[0-9A-Fa-f]{7})\s*\+\s*0x([0-9A-Fa-f]{1,5})\b")

# Addresses the mod computes from two constants, which the harvest cannot see. Each is matched on its own (a vtable
# slot: by its translated pointer), since a range over a whole vtable would be wrong where slots shifted.
COMPUTED = {
    0x14697BFA8 + 0xF0: ("renderer/family_boundary.cpp", "kContextVtable + kSubmitSlot"),
}
NAME_RX = re.compile(r"\b(k\w+)\s*(?:\[\])?\s*=\s*0x14[0-9A-Fa-f]{7}\b")


# --------------------------------------------------------------------------------------------
# Harvest
# --------------------------------------------------------------------------------------------
@dataclass
class Use:
    file: str
    line: int
    name: str
    text: str


def strip_comment(line: str, in_block: bool):
    out, i = [], 0
    while i < len(line):
        if in_block:
            j = line.find("*/", i)
            if j < 0:
                return "".join(out), True
            i, in_block = j + 2, False
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block, i = True, i + 2
            continue
        out.append(line[i])
        i += 1
    return "".join(out), in_block


def harvest(src_dir: str):
    uses: dict[int, list[Use]] = collections.defaultdict(list)
    for root, _, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith((".h", ".cpp")):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, src_dir).replace("\\", "/")
            in_block = False
            with open(path, encoding="utf-8", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    code, in_block = strip_comment(line, in_block)
                    for m in ADDR_RX.finditer(code):
                        nm = NAME_RX.search(code)
                        uses[int(m.group(1), 16)].append(Use(rel, n, nm.group(1) if nm else "", code.strip()[:160]))
                    # `0x14... + 0x...` (a vtable slot written as its vtable plus an offset): the sum is used too.
                    for m in SUM_RX.finditer(code):
                        a = int(m.group(1), 16) + int(m.group(2), 16)
                        uses[a].append(Use(rel, n, "(sum)", code.strip()[:160]))
    for a, (f, what) in COMPUTED.items():
        uses[a].append(Use(f, 0, what, "computed"))
    return uses


# --------------------------------------------------------------------------------------------
# Matching
# --------------------------------------------------------------------------------------------
@dataclass
class Range:
    old: int
    new: int
    length: int
    how: str
    masked: list = field(default_factory=list)   # (offset, size) relative to old

    def contains(self, a):
        return self.old <= a < self.old + self.length


class Matcher:
    def __init__(self, old: Image, new: Image):
        self.old, self.new = old, new
        self.ranges: list[Range] = []
        self.func_cache: dict[int, Range | None] = {}
        self.anchors: list[tuple[int, int]] = []          # (old, new) sure matches, for position prediction
        self.derived: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
        self.notes: dict[int, str] = {}

    # --- helpers ------------------------------------------------------------------------------
    def predict(self, a: int):
        if not self.anchors:
            return None
        self.anchors.sort()
        keys = [x[0] for x in self.anchors]
        i = bisect.bisect_left(keys, a)
        near = self.anchors[max(0, i - 2): i + 2]
        deltas = sorted(n - o for o, n in near)
        return a + deltas[len(deltas) // 2]

    def pick_by_position(self, a: int, hits: list[int]):
        p = self.predict(a)
        if p is None or not hits:
            return None
        ranked = sorted(hits, key=lambda h: abs(h - p))
        best = abs(ranked[0] - p)
        second = abs(ranked[1] - p) if len(ranked) > 1 else 1 << 62
        if best < 0x40000 and second > 4 * max(best, 0x800):
            return ranked[0]
        return None

    def equal_length(self, insns, a_old: int, a_new: int, end_old: int):
        """Bytes from a_old/a_new that are equal outside the masks, up to end_old."""
        pat, mask = self.old.masked_window(insns, a_old, end_old)
        nb = self.new.read(a_new, len(pat))
        n = min(len(pat), len(nb))
        for i in range(n):
            if not mask[i] and pat[i] != nb[i]:
                return i
        return n

    def equal_length_back(self, insns, a_old: int, a_new: int, begin_old: int):
        if a_old <= begin_old:
            return 0
        pat, mask = self.old.masked_window(insns, begin_old, a_old)
        nb = self.new.read(a_new - len(pat), len(pat))
        k = 0
        for i in range(len(pat) - 1, -1, -1):
            if not mask[i] and pat[i] != nb[i]:
                break
            k += 1
        # Only whole instructions count: trim to an instruction boundary.
        starts = {ins.address for ins in insns}
        while k > 0 and (a_old - k) not in starts:
            k -= 1
        return k

    def masks_in(self, insns, start, length):
        out = []
        for ins in insns:
            for o, n in ins.masked:
                p = ins.address + o - start
                if 0 <= p and p + n <= length:
                    out.append((p, n))
        return out

    def record(self, rng: Range, insns):
        rng.masked = self.masks_in(insns, rng.old, rng.length)
        self.ranges.append(rng)
        self.anchors.append((rng.old, rng.new))
        # Derived pairs: each blanked field's target in old and new.
        for ins in insns:
            if not (rng.old <= ins.address and ins.address + ins.size <= rng.old + rng.length):
                continue
            nins = ins.address - rng.old + rng.new
            for (o, t_old) in ins.targets:
                size = 8 if any(m == (o, 8) for m in ins.masked) else 4
                raw = self.new.read(nins + o, size)
                if len(raw) != size:
                    continue
                v = int.from_bytes(raw, "little", signed=(size == 4))
                t_new = v if size == 8 else nins + ins.size + v
                self.derived[t_old][t_new] += 1

    def unique_pattern_hits(self, insns, start, max_end, min_len=24, step=16, cap=1024):
        """Grow a masked pattern from `start` until unique in old; return (length, new hits)."""
        length = min_len
        while True:
            end = min(start + length, max_end)
            pat, mask = self.old.masked_window(insns, start, end)
            if sum(1 for m in mask if not m) < 8:
                return None, []
            old_hits = self.old.search(pat, mask, limit=2)
            if len(old_hits) == 1:
                return end - start, self.new.search(pat, mask, limit=4)
            if end >= max_end or length >= cap:
                # Not unique in old even at full length: return the new hits for position resolution.
                return -(end - start), self.new.search(pat, mask, limit=8)
            length += step

    # --- code ------------------------------------------------------------------------------
    def match_function(self, f):
        b, e = f
        if b in self.func_cache:
            return self.func_cache[b]
        insns = self.old.disasm(b, e)
        length, hits = self.unique_pattern_hits(insns, b, e)
        rng = None
        how = None
        target = None
        if length is not None:
            if len(hits) == 1:
                target, how = hits[0], "function"
            elif len(hits) > 1:
                target = self.pick_by_position(b, hits)
                how = "function, by position" if target else None
        if target is not None:
            L = self.equal_length(insns, b, target, e)
            if L < e - b and not self.similar_function(b, e, target, 0.7, 1.4):
                # Only the start is the same, and the function there is of another size: likely a sibling that
                # shares a prologue while the real function was recompiled (kUpdate 0x140E7A700 was matched to
                # such a sibling). Not trusted; left to the other evidence or to IDA.
                self.notes[b] = f"partial match at 0x{target:X} rejected: function size differs"
                target = None
        if target is not None:
            if L >= min(abs(length), e - b):
                rng = Range(b, target, L, how + ("" if L == e - b else f", equal for 0x{L:X} of 0x{e - b:X}"))
                self.record(rng, insns)
        self.func_cache[b] = rng
        return rng

    def match_local(self, a, f):
        b, e = f if f else (a, a + 0x200)
        insns = self.old.disasm(b, e)
        if a not in {i.address for i in insns}:
            # Not an instruction boundary from the chunk start: disassemble from a instead.
            insns = self.old.disasm(a, e)
            b = a
        length, hits = self.unique_pattern_hits(insns, a, e, min_len=16, step=8, cap=256)
        target, how = None, None
        if length is not None and len(hits) == 1 and length > 0:
            target, how = hits[0], "window at the address"
        elif length is not None and hits:
            target = self.pick_by_position(a, hits)
            how = "window at the address, by position" if target else None
        if target is None:
            # Try a window that ends at the address (code after it changed).
            starts = [i.address for i in insns if b <= i.address < a]
            for back in (16, 32, 48, 64, 96):
                s = next((x for x in reversed(starts) if a - x >= back), None)
                if s is None:
                    break
                pat, mask = self.old.masked_window(insns, s, a + 8 if a + 8 <= e else a)
                if len(self.old.search(pat, mask, limit=2)) == 1:
                    nh = self.new.search(pat, mask, limit=4)
                    if len(nh) == 1:
                        target, how = nh[0] + (a - s), "window ending at the address"
                    else:
                        t = self.pick_by_position(s, nh)
                        if t:
                            target, how = t + (a - s), "window ending at the address, by position"
                    break
        if target is None:
            return None
        of = self.old.function_at(a)
        if of and not self.similar_function(of[0], of[1], target, 0.5, 2.0):
            self.notes[a] = f"window match at 0x{target:X} rejected: the function there is of another size"
            return None
        fwd = self.equal_length(insns, a, target, e)
        back = self.equal_length_back(insns, a, target, b)
        if fwd == 0:
            return None
        rng = Range(a - back, target - back, back + fwd, how)
        self.record(rng, insns)
        return rng

    def similar_function(self, b, e, target, lo, hi):
        """True if the new function holding `target` is between lo and hi times the old function's size (or either
        has no unwind data, so no size to compare)."""
        nf = self.new.function_at(target)
        if nf is None or e <= b:
            return True
        ratio = (nf[1] - nf[0]) / (e - b)
        return lo <= ratio <= hi

    def find_range(self, a):
        for r in self.ranges:
            if r.contains(a):
                return r
        return None

    def match_code(self, a):
        r = self.find_range(a)
        if r:
            return r
        f = self.old.function_at(a)
        if f:
            r = self.match_function(f)
            if r and r.contains(a) and (a - r.old) < r.length:
                return r
        return self.match_local(a, f)

    # --- data ------------------------------------------------------------------------------
    def derived_for(self, d):
        c = self.derived.get(d)
        if c:
            (t, n), *rest = c.most_common()
            return t, n, sum(c.values())
        return None

    def scan_refs(self, targets: set[int], spread=0x40):
        """Positions in old .text whose 4 bytes, read as a RIP-relative disp32 ending the instruction
        (or followed by an 8/16/32-bit immediate), point at or just before a target."""
        _, tstart, _, traw, trawsize = self.old.text
        buf = np.frombuffer(self.old.data, dtype=np.uint8, count=trawsize, offset=traw)
        want = np.array(sorted(targets), dtype=np.int64)
        found = collections.defaultdict(list)
        chunk = 1 << 23
        for c0 in range(0, trawsize - 4, chunk):
            c1 = min(trawsize - 4, c0 + chunk)
            b = buf[c0:c1 + 4].astype(np.int64)
            n = c1 - c0
            disp = b[0:n] | (b[1:n + 1] << 8) | (b[2:n + 2] << 16) | (b[3:n + 3] << 24)
            disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
            pos = np.arange(c0, c1, dtype=np.int64) + tstart
            for tail in (0, 1, 2, 4):
                t = pos + 4 + tail + disp
                idx = np.searchsorted(want, t, side="right") - 1
                ok = idx >= 0
                idx2 = np.where(ok, idx, 0)
                delta = t - want[idx2]
                hit = ok & (delta >= -spread) & (delta <= 0)
                for p in np.nonzero(hit)[0]:
                    found[int(want[idx2[p]])].append((int(pos[p]), int(t[p])))
        return found

    def pointer_slots(self, value: int, near: int | None):
        """IDA addresses in new .rdata/.data holding the 8-byte value."""
        needle = value.to_bytes(8, "little")
        out = []
        for name, start, vsize, raw, rawsize in self.new.sections:
            if name not in (".rdata", ".data"):
                continue
            i = self.new.data.find(needle, raw, raw + rawsize)
            while i >= 0:
                if (i - raw) % 8 == 0:
                    out.append(start + (i - raw))
                i = self.new.data.find(needle, i + 1, raw + rawsize)
        return out

    def code_pointer_run(self, img: Image, va):
        """Start and end of the run of consecutive code pointers around va (a vtable, roughly)."""
        def is_code(p):
            return p is not None and img.section_of(p) == ".text"
        s = va
        while is_code(img.u64(s - 8)):
            s -= 8
        e = va
        while is_code(img.u64(e)):
            e += 8
        return s, e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--version-name", default="1.006")
    ap.add_argument("--source-name", default="1.005", help="the game version the mod's addresses are written for")
    ap.add_argument("--hand", default=None, help="folder with the IDA agents' replaced-*.json (hand-found addresses)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    old, new = Image(args.old), Image(args.new)
    m = Matcher(old, new)
    uses = harvest(args.src)
    addrs = sorted(uses)
    print(f"harvested {len(addrs)} addresses in code", flush=True)

    results = {}
    code = [a for a in addrs if old.section_of(a) == ".text"]
    data = [a for a in addrs if a not in set(code)]

    # Pass 1 (unique only) then pass 2 (position resolution has anchors from pass 1).
    for pass_no in (1, 2):
        for a in code:
            if a in results and results[a]["new"] is not None:
                continue
            r = m.match_code(a)
            if r:
                results[a] = {"kind": "code", "new": a - r.old + r.new, "how": r.how, "range": [r.old, r.new, r.length]}
            else:
                results[a] = {"kind": "code", "new": None, "how": "not found"}
        print(f"pass {pass_no}: code matched {sum(1 for a in code if results[a]['new'] is not None)}/{len(code)}", flush=True)

    # Data: derived pairs first.
    def resolve_data(a):
        d = m.derived_for(a)
        if d and d[1] * 2 > d[2]:
            return d[0], f"referenced by matched code ({d[1]}/{d[2]} refs agree)"
        # Same object as a referenced address just below it.
        for k in range(8, 0x48, 8):
            d = m.derived_for(a - k)
            if d and d[1] * 2 > d[2]:
                return d[0] + k, f"0x{k:X} past an address matched code references"
        return None, None

    pending = []
    for a in data:
        n, how = resolve_data(a)
        results[a] = {"kind": "data", "new": n, "how": how or "not found", "section": old.section_of(a)}
        if n is None:
            pending.append(a)
    print(f"data from derived refs: {len(data) - len(pending)}/{len(data)}", flush=True)

    # Data: scan old code for references, match those functions, derive again.
    if pending:
        refs = m.scan_refs(set(pending))
        for a in pending:
            for pos, _ in refs.get(a, [])[:6]:
                f = old.function_at(pos)
                if f:
                    m.match_function(f)
        still = []
        for a in pending:
            n, how = resolve_data(a)
            if n is not None:
                results[a].update(new=n, how=how + " (found by scan)")
            else:
                still.append(a)
        pending = still
        print(f"data after scan: {len(data) - len(pending)}/{len(data)}", flush=True)

    # Data: slots holding code pointers (vtables). A function re-found by hand counts as matched.
    hand = dict(load_hand_found(args.hand))
    for a in list(pending):
        p = old.u64(a)
        if p is None or old.section_of(p) != ".text":
            continue
        r = m.match_code(p)
        if r:
            p_new = p - r.old + r.new
        elif p in hand:
            p_new = hand[p]
        else:
            results[a]["how"] = f"slot holds 0x{p:X}, which did not match"
            continue
        cands = m.pointer_slots(p_new, None)
        chosen = None
        if len(cands) == 1:
            chosen, how = cands[0], "slot found by its translated pointer"
        elif cands:
            # Neighbours decide: count how many adjacent slots also translate.
            def score(c):
                s = 0
                for k in range(-6, 7):
                    if k == 0:
                        continue
                    po = old.u64(a + 8 * k)
                    if po is None or old.section_of(po) != ".text":
                        continue
                    rr = m.match_code(po)
                    if rr and new.u64(c + 8 * k) == po - rr.old + rr.new:
                        s += 1
                return s
            scored = sorted(((score(c), c) for c in cands), reverse=True)
            if scored[0][0] >= 3 and (len(scored) == 1 or scored[1][0] < scored[0][0]):
                chosen, how = scored[0][1], f"slot found by its translated pointer and {scored[0][0]} neighbours"
        if chosen:
            results[a].update(new=chosen, how=how)
            pending.remove(a)

    # vtable runs: is the slot at the same index of its pointer run in both builds?
    for a in data:
        res = results[a]
        p = old.u64(a)
        if res["new"] is None or p is None or old.section_of(p) != ".text":
            continue
        os_, oe = m.code_pointer_run(old, a)
        ns, ne = m.code_pointer_run(new, res["new"])
        res["run"] = {"old_index": (a - os_) // 8, "old_len": (oe - os_) // 8,
                      "new_index": (res["new"] - ns) // 8, "new_len": (ne - ns) // 8}

    # Cross-check code matches against derived pairs (calls from other matched code). Where every caller agrees
    # on another address, the callers win if the function's bytes match there too (the own match then found an
    # identical copy elsewhere); else the address is dropped as ambiguous.
    conflicts = []
    for a in code:
        n = results[a]["new"]
        d = m.derived_for(a)
        if n is not None and d and d[0] != n:
            conflicts.append((a, n, d[0], d[1], d[2]))
            f = old.function_at(a) or (a, a + 0x100)
            insns = old.disasm(f[0], f[1])
            L = m.equal_length(insns, a, d[0], f[1])
            bad = [r for r in m.ranges if r.contains(a) and r.new + (a - r.old) == n]
            for r in bad:
                m.ranges.remove(r)
            if d[1] >= 3 and d[1] == d[2] and L >= min(24, f[1] - a):
                rng = Range(a, d[0], L, "function, as its callers say")
                m.record(rng, insns)
                results[a].update(new=d[0], how=rng.how, range=[rng.old, rng.new, rng.length])
            else:
                results[a].update(new=None, how=f"ambiguous: own match 0x{n:X}, callers 0x{d[0]:X}")
        if n is None and d and d[1] * 2 > d[2]:
            # The callers say where it went. If its bytes there are the same (masked), it is as good as a match
            # of its own: record the range, so byte checks at it pass.
            f = old.function_at(a) or (a, a + 0x40)
            insns = old.disasm(a, f[1])
            L = m.equal_length(insns, a, d[0], f[1])
            if L >= min(24, f[1] - a):
                rng = Range(a, d[0], L, f"function where its callers say ({d[1]}/{d[2]} agree)")
                m.record(rng, insns)
                results[a].update(new=d[0], how=rng.how, range=[rng.old, rng.new, rng.length])
            else:
                results[a].update(new=d[0], how=f"call target from matched code ({d[1]}/{d[2]} agree)")

    # Structural tier, for the report only: where code that did not match went, and what changed in it.
    for a in code:
        if results[a]["new"] is None or results[a]["how"].startswith("call target"):
            results[a]["structural"] = structural_diff(old, new, m, a)

    # vtable starts (lea targets) for every slot the mod names.
    lea_old, lea_new = lea_targets(old), lea_targets(new)
    for a in data:
        res = results[a]
        if res["new"] is None or "run" not in res:
            continue
        vo = nearest_below(lea_old, a)
        vn = nearest_below(lea_new, res["new"])
        if vo and vn:
            res["vtable"] = {"old": vo, "new": vn, "old_index": (a - vo) // 8, "new_index": (res["new"] - vn) // 8,
                             "start_maps": m.derived_for(vo)[0] == vn if m.derived_for(vo) else None}
    # Hand-found addresses (the IDA agents' replaced-*.json in --hand) win: a range that maps one of them elsewhere
    # is wrong, and is dropped with every address it carried.
    contradicted = []
    for old_a, new_a in load_hand_found(args.hand):
        for r in [r for r in m.ranges if r.contains(old_a) and r.new + (old_a - r.old) != new_a]:
            m.ranges.remove(r)
            contradicted.append((old_a, new_a, r.old, r.new, r.length))
            for a, res in results.items():
                if res.get("range") == [r.old, r.new, r.length]:
                    res.update(new=None, how=f"dropped: its range contradicts the hand-found 0x{old_a:X} -> 0x{new_a:X}")
    for c in contradicted:
        print(f"range 0x{c[2]:X} -> 0x{c[3]:X} (+0x{c[4]:X}) dropped: hand-found 0x{c[0]:X} -> 0x{c[1]:X}")
    write_outputs(args, old, new, m, uses, results, conflicts)


def load_hand_found(directory):
    pairs = []
    if not directory:
        return pairs
    import glob
    for path in glob.glob(os.path.join(directory, "replaced-*.json")):
        data = json.load(open(path, encoding="utf-8"))
        items = data if isinstance(data, list) else (data.get("items") or data.get("facts") or [])
        for it in items:
            try:
                pairs.append((int(it["old"], 16), int(it["new"], 16)))
            except (KeyError, ValueError, TypeError):
                pass
    return pairs


def lea_targets(img: Image):
    """Sorted targets of every `lea r64, [rip+disp32]` in .text that land in .rdata (vtables, tables)."""
    _, tstart, _, traw, trawsize = img.text
    rd = next(s for s in img.sections if s[0] == ".rdata")
    rx = re.compile(rb"[\x48\x4C]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]", re.DOTALL)
    out = set()
    data = img.data
    for mt in rx.finditer(data, traw, traw + trawsize):
        p = mt.start() + 3
        disp = int.from_bytes(data[p:p + 4], "little", signed=True)
        t = tstart + (p - traw) + 4 + disp
        if rd[1] <= t < rd[1] + rd[2] and t % 8 == 0:
            out.add(t)
    return sorted(out)


def nearest_below(sorted_list, a, window=0x8000):
    i = bisect.bisect_right(sorted_list, a) - 1
    return sorted_list[i] if i >= 0 and a - sorted_list[i] < window else None


def structural_diff(old: Image, new: Image, m: Matcher, a: int):
    """Match the function holding `a` with every displacement and immediate blanked; list the instructions whose
    unblanked-in-the-strict-sense bytes differ (struct offsets, vtable slots, constants)."""
    import capstone
    f = old.function_at(a)
    if not f:
        return {"found": None}
    b, e = f
    insns = old.disasm(b, e)
    found = None
    for n in (32, 48, 64, 96, 128, 192, 256, 384, 512):
        end = min(b + n, e)
        pat, mask = old.masked_window(insns, b, end, loose=True)
        if sum(1 for x in mask if not x) < 12:
            continue
        oh = old.search(pat, mask, limit=2)
        if len(oh) == 1:
            nh = new.search(pat, mask, limit=4)
            found = nh[0] if len(nh) == 1 else m.pick_by_position(b, nh)
            break
        if end == e:
            break
    if not found:
        return {"found": None}
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    ninsns = new.disasm(found, found + (e - b) + 64)
    changes, diverged = [], None
    for oi, ni in zip(insns, ninsns):
        ob, nb = old.read(oi.address, oi.size), new.read(ni.address, ni.size)
        if oi.size != ni.size or ob[:1] != nb[:1]:
            diverged = oi.address - b
            break
        diff = [k for k in range(oi.size) if ob[k] != nb[k] and not any(o <= k < o + s for o, s in oi.masked)]
        if diff:
            ot, nt = next(cs.disasm(ob, oi.address)), next(cs.disasm(nb, ni.address))
            changes.append(f"+0x{oi.address - b:X}: `{ot.mnemonic} {ot.op_str}` → `{nt.mnemonic} {nt.op_str}`")
    return {"found": found, "at": found + (a - b), "changes": changes[:12], "n_changes": len(changes),
            "diverged_at": diverged, "size": e - b}


DATA_EXTENT = 0x1000  # how far past a matched static object its fields are translated with it


def slot_signature(img: Image, p: int, n: int = 48) -> bytes:
    """A function's masked first bytes (leaf stubs up to their first ret/jmp), to compare vtable slots across builds."""
    f = img.function_at(p)
    end = min(f[1], p + n) if f else p + 16
    ins = img.disasm(p, end)
    if not f:
        for k in ins:
            if img.read(k.address, 1) in (b"\xc3", b"\xe9", b"\xeb"):
                end = k.address + k.size
                break
        ins = [k for k in ins if k.address < end]
    b, mask = img.masked_window(ins, p, end)
    return bytes(0 if mk else x for x, mk in zip(b, mask))


def aligned_slots(m: "Matcher", vo: int, vn: int, hand: dict, limit: int = 400) -> int:
    """How many leading slots of the vtables at vo (old) and vn (new) line up: up to the first slot whose old function
    is known to be another function in the new build. A slot agrees when its function's masked bytes are the same or
    its old function translates (matched, or re-found by hand) to the new slot's; a recompiled function nobody matched
    is unknown and neither confirms nor breaks the line. 0 unless most of the compared slots agree."""
    old, new = m.old, m.new
    k = agree = 0
    while k < limit:
        po, pn = old.u64(vo + 8 * k), new.u64(vn + 8 * k)
        if po is None or pn is None or old.section_of(po) != ".text" or new.section_of(pn) != ".text":
            break
        if slot_signature(old, po) == slot_signature(new, pn):
            agree += 1
        else:
            r = m.match_code(po)
            tr = po - r.old + r.new if r else hand.get(po)
            if tr is not None:
                if tr != pn:
                    break  # a known function in another slot: the layouts part here (e.g. UObject slot 19)
                agree += 1
        k += 1
    return k if k and agree * 2 > k else 0


def table_ranges(m: Matcher, results, old: Image = None, new: Image = None, hand: dict = None):
    """The ranges for the C++ table: every matched code range; per matched data address, a range over its object
    (a static object in .data/.bss: up to DATA_EXTENT, cut at the next address with another delta; a vtable start:
    its leading slots that hold the same functions in both builds; else 8 bytes); a 1-byte range per code address
    known only from its callers. Sorted, merged where they overlap with the same delta. Where they overlap with a
    different delta, a data extent gives way (it ends where the other range starts); otherwise the earlier range wins
    (reported)."""
    items = [(r.old, r.new, r.length, list(r.masked), False) for r in m.ranges]
    for a, res in results.items():
        if res["new"] is None:
            continue
        if res["kind"] == "data":
            sec = old.section_of(a) if old else None
            p = old.u64(a) if old else None
            if p is not None and old.section_of(p) == ".text" and res.get("vtable") and res["vtable"]["old"] == a:
                k = aligned_slots(m, a, res["new"], hand or {})
                res["aligned_slots"] = k
                items.append((a, res["new"], max(8, 8 * k), [], False))
            elif sec in (".data",) or sec is None:
                items.append((a, res["new"], DATA_EXTENT, [], True))
            else:
                items.append((a, res["new"], 8, [], False))
        elif res["how"].startswith("call target"):
            items.append((a, res["new"], 1, [], False))
    items.sort()
    out, dropped = [], []
    for o, n, L, fields, soft in items:
        if out:
            po, pn, pL, pf, psoft = out[-1]
            if o < po + pL:
                if n - o == pn - po:
                    end = max(po + pL, o + L)
                    shift = o - po
                    merged = {(fo, fs) for fo, fs in pf} | {(fo + shift, fs) for fo, fs in fields}
                    out[-1] = (po, pn, end - po, sorted(merged), psoft and soft)
                elif psoft and o > po:
                    # A static object's extent gives way to the next matched address.
                    out[-1] = (po, pn, o - po, [f for f in pf if f[0] + f[1] <= o - po], psoft)
                    out.append((o, n, L, fields, soft))
                else:
                    if soft:
                        if o + L > po + pL:
                            cut = po + pL - o
                            out.append((o + cut, n + cut, L - cut, [], soft))
                        continue
                    if o + L > po + pL and o >= po + 1:
                        # Keep the earlier range; the later one only from where the earlier ends.
                        cut = po + pL - o
                        fields2 = [(fo - cut, fs) for fo, fs in fields if fo >= cut]
                        if L - cut > 0:
                            out.append((o + cut, n + cut, L - cut, fields2, soft))
                    dropped.append((o, n, L, po, pn, pL))
                continue
        out.append((o, n, L, fields, soft))
    return [(o, n, L, f) for o, n, L, f, _ in out], dropped


def write_header(path, version, ranges, source_name):
    ns = "v" + version.replace(".", "_")
    flat, rows = [], []
    for o, n, L, fields in ranges:
        rows.append(f"    {{0x{o - IDA_BASE:08X}, 0x{n - IDA_BASE:08X}, 0x{L:X}, {len(flat)}, {len(fields)}}},")
        flat.extend(fields)
    lines = [
        "#pragma once",
        "",
        f"// Generated by tools/version_match/match.py (source build {source_name} -> {version}). Do not edit: rerun the tool.",
        "// Each range: source RVA, this build's RVA, length, first moved field, moved field count.",
        "",
        '#include "platform/build_translation.h"',
        "",
        "#include <array>",
        "",
        f"namespace lss::platform::builds::{ns} {{",
        "",
        f"inline constexpr std::array<TranslatedRange, {len(rows)}> kRanges{{{{",
        *rows,
        "}};",
        "",
        f"inline constexpr std::array<MovedField, {len(flat)}> kFields{{{{",
        *[f"    {{0x{fo:X}, {fs}}}," for fo, fs in flat],
        "}};",
        "",
        f"}}  // namespace lss::platform::builds::{ns}",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def write_outputs(args, old, new, m, uses, results, conflicts):
    ranges, dropped = table_ranges(m, results, old, new, dict(load_hand_found(args.hand)))
    header = os.path.join(args.out, f"build_{args.version_name.replace('.', '_')}.h")
    write_header(header, args.version_name, ranges, args.source_name)
    # The table must reproduce every match.
    by_old = sorted(ranges)
    for a, res in results.items():
        if res["new"] is None:
            continue
        i = bisect.bisect_right([r[0] for r in by_old], a) - 1
        ok = i >= 0 and by_old[i][0] <= a < by_old[i][0] + by_old[i][2] and by_old[i][1] + (a - by_old[i][0]) == res["new"]
        res["in_table"] = bool(ok)
    addrs = sorted(results)
    matched = [a for a in addrs if results[a]["new"] is not None]
    missing = [a for a in addrs if results[a]["new"] is None]
    shifted_runs = [a for a in addrs if "run" in results[a] and results[a]["run"]["old_index"] != results[a]["run"]["new_index"]]

    with open(os.path.join(args.out, "matches.json"), "w") as f:
        json.dump({hex(a): {**results[a], "new": hex(results[a]["new"]) if results[a]["new"] else None,
                            "uses": [u.__dict__ for u in uses[a]]} for a in addrs}, f, indent=1)

    lines = [f"# Address matching {os.path.basename(os.path.dirname(args.old))} → {args.version_name}", "",
             f"- addresses in the mod's code: **{len(addrs)}** ({sum(1 for a in addrs if results[a]['kind'] == 'code')} code, "
             f"{sum(1 for a in addrs if results[a]['kind'] == 'data')} data)",
             f"- matched: **{len(matched)}**; not found: **{len(missing)}**",
             f"- matched ranges: {len(m.ranges)}; code-match conflicts with call targets: {len(conflicts)}",
             f"- C++ table: {len(ranges)} ranges, {sum(len(r[3]) for r in ranges)} moved fields; overlaps with a "
             f"different delta (later range cut): {len(dropped)}; matches the table does not reproduce: "
             f"{sum(1 for a in matched if not results[a].get('in_table'))}", ""]
    how = collections.Counter(results[a]["how"].split(" (")[0].split(",")[0] for a in matched)
    lines.append("| how | count |")
    lines.append("|---|---|")
    for k, v in how.most_common():
        lines.append(f"| {k} | {v} |")
    lines += ["", "## Not found", "", "| address | kind | names / files | note |", "|---|---|---|---|"]
    for a in missing:
        u = uses[a]
        names = ", ".join(sorted({x.name for x in u if x.name})) or "-"
        files = ", ".join(sorted({x.file for x in u}))
        lines.append(f"| 0x{a:X} | {results[a]['kind']} | {names} ({files}) | {results[a]['how']} |")
    lines += ["", "## Where unmatched code went (structural match: displacements and immediates blanked)", "",
              "Only for the report: these are **not** in the table. `changes` are instructions whose struct offsets, "
              "vtable slots or constants differ; `diverged` is where the instruction stream itself changed.", ""]
    for a in addrs:
        s = results[a].get("structural")
        if not s:
            continue
        names = ", ".join(sorted({x.name for x in uses[a] if x.name})) or "-"
        if s.get("found") is None:
            lines.append(f"- 0x{a:X} ({names}): no structural match")
            continue
        div = f"; diverges at +0x{s['diverged_at']:X} of 0x{s['size']:X}" if s["diverged_at"] is not None else ""
        lines.append(f"- 0x{a:X} ({names}) → likely 0x{s['at']:X} (function 0x{s['found']:X}): "
                     f"{s['n_changes']} changed instructions{div}")
        for c in s["changes"]:
            lines.append(f"  - {c}")
    lines += ["", "## vtable slot indices (vtable start = nearest `lea` target at or below the slot)", "",
              "| slot old | slot new | vtable old → new | index old → new | start maps | names |", "|---|---|---|---|---|---|"]
    for a in addrs:
        v = results[a].get("vtable")
        if not v:
            continue
        names = ", ".join(sorted({x.name for x in uses[a] if x.name})) or "-"
        flag = "" if v["old_index"] == v["new_index"] else " **moved**"
        lines.append(f"| 0x{a:X} | 0x{results[a]['new']:X} | 0x{v['old']:X} → 0x{v['new']:X} | "
                     f"{v['old_index']} → {v['new_index']}{flag} | {v['start_maps']} | {names} |")
    lines += ["", "## Conflicts (own match vs call targets from other matched code)", ""]
    for a, n, d, k, t in conflicts:
        lines.append(f"- 0x{a:X}: own 0x{n:X}, callers say 0x{d:X} ({k}/{t})")
    lines += ["", "## All matches", "", "| old | new | delta | kind | how | names |", "|---|---|---|---|---|---|"]
    for a in matched:
        r = results[a]
        names = ", ".join(sorted({x.name for x in uses[a] if x.name})) or "-"
        lines.append(f"| 0x{a:X} | 0x{r['new']:X} | {r['new'] - a:+#x} | {r['kind']} | {r['how']} | {names} |")
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"matched {len(matched)}/{len(addrs)}; missing {len(missing)}; conflicts {len(conflicts)}; shifted vtable slots {len(shifted_runs)}")


if __name__ == "__main__":
    main()
