#!/usr/bin/env python3
"""Who calls this function?

Scans .text for every BL/BLX -- Thumb and ARM -- and reports the ones that
target a named symbol. Built for a specific question ("what is supposed to call
startClet?") but the same question comes up constantly when a port has to hook
a function the engine never reaches: the Vita port's patch.c is largely a list
of functions it decided to intercept, and finding them is this.

Direct branches only. A call through a function pointer or a PLT slot will not
appear here -- if a symbol shows no callers, that is the first thing to suspect.

Usage:
    tools/callers.py <libgame.so> <symbol> [<symbol> ...]
    tools/callers.py <libgame.so> --all-targets-of <addr-hex>
"""

import struct
import subprocess
import sys

TEXT_VA_DEFAULT = None  # read from the section headers


def read_sections(path):
    """Section name -> (vaddr, offset, size). Parsed from readelf -S."""
    out = subprocess.run(["readelf", "-S", "-W", path],
                         capture_output=True, text=True).stdout
    secs = {}
    for line in out.splitlines():
        # "[ 1] .dynsym DYNSYM 00000114 000114 01ed80 10 A 2 1 4"
        # readelf pads the index, so "[ 1]" splits into two tokens while
        # "[10]" does not -- the columns shift depending on the width.
        parts = line.split()
        if not parts or not parts[0].startswith("["):
            continue
        shift = 1 if parts[0] == "[" else 0
        if len(parts) < 6 + shift:
            continue
        try:
            name = parts[1 + shift]
            addr = int(parts[3 + shift], 16)
            off = int(parts[4 + shift], 16)
            size = int(parts[5 + shift], 16)
        except (ValueError, IndexError):
            continue
        secs[name] = (addr, off, size)
    return secs


def read_symbols(path):
    out = subprocess.run(["readelf", "--dyn-syms", "-W", path],
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) < 8 or p[3] != "FUNC" or p[6] == "UND":
            continue
        try:
            syms[p[7]] = int(p[1], 16) & ~1
        except ValueError:
            continue
    return syms


def nearest(syms_sorted, addr):
    lo, hi = 0, len(syms_sorted)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms_sorted[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    return syms_sorted[lo - 1]


def thumb_bl_target(text, i, va):
    """Decode a Thumb BL/BLX at text[i:]; returns (target, width) or None."""
    if i + 4 > len(text):
        return None
    h = struct.unpack_from("<H", text, i)[0]
    if (h & 0xF800) != 0xF000:
        return None
    lo = struct.unpack_from("<H", text, i + 2)[0]
    # bits[15:14] = 11 for both BL and BLX; bit 12 tells them apart (BL=1).
    if (lo & 0xC000) != 0xC000:
        return None
    S = (h >> 10) & 1
    imm10 = h & 0x3FF
    J1 = (lo >> 13) & 1
    J2 = (lo >> 11) & 1
    imm11 = lo & 0x7FF
    I1 = (~(J1 ^ S)) & 1
    I2 = (~(J2 ^ S)) & 1
    v = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)
    v = struct.unpack("<i", struct.pack("<I", (v << 7) & 0xFFFFFFFF))[0]
    return (va + 4 + v) & 0xFFFFFFFF, 4


def arm_bl_target(text, i, va):
    """Decode an ARM BL/BLX at text[i:] (4-byte aligned)."""
    if i + 4 > len(text):
        return None
    w = struct.unpack_from("<I", text, i)[0]
    if (w >> 28) != 0xE and (w >> 28) != 0xF:
        pass  # conditional branches are still BL/BLX
    top = (w >> 24) & 0xF
    if top not in (0xA, 0xB):
        return None
    off = w & 0xFFFFFF
    off = (off << 8) >> 6
    return (va + 8 + off) & 0xFFFFFFFF, 4


def scan(path):
    secs = read_sections(path)
    if ".text" not in secs:
        raise SystemExit("no .text section")
    text_va, text_off, text_sz = secs[".text"]
    with open(path, "rb") as f:
        blob = f.read()
    text = blob[text_off:text_off + text_sz]

    calls = []          # (call_site_va, target_va)
    i = 0
    while i + 4 <= len(text):
        va = text_va + i
        r = thumb_bl_target(text, i, va)
        if r:
            calls.append((va, r[0]))
            i += r[1]
            continue
        r = arm_bl_target(text, i, va)
        if r:
            calls.append((va, r[0]))
            i += r[1]
            continue
        i += 2
    return calls, text_va, text_sz


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, args = sys.argv[1], sys.argv[2:]
    calls, text_va, text_sz = scan(path)
    syms = read_symbols(path)
    syms_sorted = sorted((v, k) for k, v in syms.items())
    print(f"scanned .text {text_va:#x}..{text_va+text_sz:#x}: "
          f"{len(calls)} BL/BLX sites, {len(syms)} FUNC symbols")

    if args[0] == "--all-targets-of":
        want = {int(args[1], 16) & ~1}
    else:
        want = set()
        for name in args:
            if name not in syms:
                print(f"  !! no such symbol: {name}")
                continue
            want.add(syms[name])
            print(f"  {name} @ {syms[name]:#x}")

    hits = {}
    for site, tgt in calls:
        if (tgt & ~1) in want:
            hits.setdefault(tgt & ~1, []).append(site)

    if not hits:
        print("\nno direct BL/BLX targets any of those. Possible reasons:")
        print("  - it is called through a function pointer, a vtable, or a PLT slot")
        print("  - it is reached by a computed branch (MOV PC / LDR PC / jump table)")
        print("  - nothing in this binary calls it at all (it is an entry point)")
        return 0

    for tgt, sites in hits.items():
        names = [n for n, v in syms.items() if v == tgt]
        print(f"\n{names[0] if names else hex(tgt)} @ {tgt:#x}  "
              f"-- {len(sites)} call site(s):")
        for s in sorted(sites):
            n = nearest(syms_sorted, s)
            label = f"{n[1]}+0x{s - n[0]:x}" if n else "?"
            print(f"    {s:#08x}  {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
