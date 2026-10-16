#!/usr/bin/env python3
"""Differential-testing oracle for the hand-written ARM/Thumb interpreter.

Runs the *same* guest memory image the C spike built -- it consumes the dump
written by `--dump-guest` rather than doing its own relocation -- under Unicorn,
and emits the same per-instruction trace format. Feeding both traces to
tools/difftrace.py names the first instruction whose semantics differ.

Why this shape:

  * Sharing the memory image removes the loader from the comparison. A
    divergence is then a CPU bug by construction, not "maybe the relocation
    differed".
  * Unicorn is an oracle, not the product. Its speed comes from QEMU's TCG JIT,
    and a browser cannot execute generated machine code -- the wasm ports run
    TCI, a two-level interpreter that does strictly more work per guest
    instruction than a direct decoder. So it is the right tool for checking
    semantics and the wrong tool for shipping.
  * Stopping at the first host crossing bounds the comparison to pure CPU
    semantics: neither side's shims can explain a difference in the traces.

Usage:
    .venv/bin/python3 tools/oracle.py <guest.bin> <out.trace> [entry_hex] [budget]
"""

import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R13, UC_ARM_REG_R14,
    UC_ARM_REG_R15, UC_ARM_REG_CPSR,
)

# Must match src/guest.h.
GUEST_IMAGE_BASE    = 0x01000000
GUEST_STACK_TOP     = 0x06800000
GUEST_STACK_SIZE    = 0x00800000
GUEST_STUB_BASE     = 0x08800000
GUEST_STUB_SIZE     = 0x00400000
GUEST_MEM_SIZE      = 0x09000000
# Must match GUEST_STUB_BASE and IMPORT_MAX in src/. A stub address encodes its
# import's index, which is how a crossing is named here.
GUEST_STUB_BASE     = 0x08800000
IMPORT_MAX          = 1024

# The imports both sides perform identically. Must match import_is_reproducible()
# in src/imports.c -- a name on one list and not the other is a divergence that
# is the harness's fault, not the decoder's.
REPRODUCIBLE = {
    "memcpy", "memmove", "memset", "memcmp",
    "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
}
# The executable range of the image; src/elf_loader.c derives the same bounds
# from the PT_LOAD marked PF_X. Printed by the C spike at load time.
TEXT_LO             = 0x01000000
TEXT_HI             = 0x0113dac4
GUEST_INIT_ARRAY    = GUEST_IMAGE_BASE + 0x13ecfc
# Must match RETURN_SENTINEL in src/main.c. Not 0: a guest that
# branches through a null function pointer also lands on 0.
RETURN_SENTINEL     = 0x0BADF00D


def load_dump(path):
    """Read the GIMG dump: (addr, size) pairs and their bytes, then the import
    name table the C side appends under its own magic.

    The names are what let a stub address be resolved to something meaningful.
    Without them the only options are to stop at the first crossing or to
    reproduce every import blind -- and the second is how a differential starts
    reporting divergences that are the harness's fault.
    """
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"GIMG":
        raise SystemExit("not a GIMG dump: %s" % path)
    n = struct.unpack_from("<I", blob, 4)[0]
    regions = []
    off = 8
    for _ in range(n):
        addr, size = struct.unpack_from("<II", blob, off)
        off += 8
        regions.append((addr, size))
    out = []
    for addr, size in regions:
        out.append((addr, blob[off:off + size]))
        off += size

    regs = None
    if blob[off:off + 4] == b"REGS":
        off += 4
        regs = list(struct.unpack_from("<16I", blob, off))
        off += 64
        regs.append(struct.unpack_from("<I", blob, off)[0])   # cpsr
        off += 4

    names = []
    if blob[off:off + 4] == b"INAM":
        off += 4
        ni = struct.unpack_from("<I", blob, off)[0]
        off += 4
        for _ in range(ni):
            ln = struct.unpack_from("<I", blob, off)[0]
            off += 4
            names.append(blob[off:off + ln].decode("latin1"))
            off += ln
    return out, names, regs


def do_reproducible(uc, name, r):
    """Perform one of the imports both sides implement, and return the value for
    r0 -- or None if this name is not one of them.

    These must match src/host_libc.c byte for byte. A difference here is a
    divergence the harness caused, which is worse than not running the import at
    all: it would be chased as a decoder bug.
    """
    def mem(a, n):
        return bytes(uc.mem_read(a, n))

    if name == "memcpy" or name == "memmove":
        dst, src, n = r[0], r[1], r[2]
        if dst and src and n:
            uc.mem_write(dst, mem(src, n))
        return dst
    if name == "memset":
        dst, c, n = r[0], r[1], r[2]
        if dst and n:
            uc.mem_write(dst, bytes([c & 0xFF]) * n)
        return dst
    if name == "memcmp":
        a, b, n = r[0], r[1], r[2]
        if not (a and b) or not n:
            return 0
        x, y = mem(a, n), mem(b, n)
        d = 0
        for i in range(n):
            if x[i] != y[i]:
                d = x[i] - y[i]
                break
        return d & 0xFFFFFFFF
    if name in ("strlen", "strcmp", "strncmp", "strcpy", "strncpy"):
        def cstr(a, cap=1 << 20):
            out = bytearray()
            while len(out) < cap:
                c = uc.mem_read(a + len(out), 1)[0]
                if c == 0:
                    break
                out.append(c)
            return bytes(out)
        if name == "strlen":
            return len(cstr(r[0])) if r[0] else 0
        if name == "strcmp":
            x, y = cstr(r[0]), cstr(r[1])
            n = min(len(x), len(y))
            for i in range(n):
                if x[i] != y[i]:
                    return (x[i] - y[i]) & 0xFFFFFFFF
            return (len(x) - len(y)) & 0xFFFFFFFF
        if name == "strncmp":
            x, y, n = cstr(r[0]), cstr(r[1]), r[2]
            i = 0
            while i < n:
                xa = x[i] if i < len(x) else 0
                ya = y[i] if i < len(y) else 0
                if xa != ya:
                    return (xa - ya) & 0xFFFFFFFF
                if xa == 0:
                    break
                i += 1
            return 0
        if name == "strcpy" or name == "strncpy":
            dst, src = r[0], r[1]
            s = cstr(src)
            if name == "strcpy":
                if dst:
                    uc.mem_write(dst, s + b"\x00")
            else:
                n = r[2]
                if dst:
                    body = s[:n] + b"\x00" * max(0, n - len(s))
                    uc.mem_write(dst, body[:n])
            return dst
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_path, trace_path = sys.argv[1], sys.argv[2]
    entry = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 20_000_000

    regions, import_names, snap = load_dump(dump_path)

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)

    # The whole guest address space is mapped, not just the dumped regions.
    # The interpreter's space is one flat buffer, so a stray write to address 0
    # (which happens as soon as an import returns NULL) lands in the null guard
    # region and is harmless. Unicorn would instead raise UC_ERR_WRITE_UNMAPPED
    # and end the comparison early -- a divergence caused by the harness rather
    # than by the decoder. Same memory model on both sides, or the diff lies.
    uc.mem_map(0, GUEST_MEM_SIZE)
    for addr, data in regions:
        uc.mem_write(addr, data)

    # Pick the entry the same way src/main.c does for ctor[0].
    if entry is None:
        raw = uc.mem_read(GUEST_INIT_ARRAY, 4)
        entry = struct.unpack("<I", bytes(raw))[0] | 1

    # A dump taken at a chosen address carries the engine's real state there.
    # Starting from it is the only way to diff a deep function: inflate entered
    # with no arguments decides nothing about whether inflate is correct, and
    # reconstructing a z_stream by hand would be a harness that could be wrong
    # in the same direction as the thing under test.
    if snap is not None:
        entry = snap[15] | 1
        print("oracle: resuming from a snapshot at %08x" % (entry & ~1))

    out = open(trace_path, "w", buffering=1 << 20)
    state = {"prev": None, "stopped": False, "count": 0}

    def read_regs():
        # r0-r14 and CPSR. r15 is deliberately omitted: the interpreter's R[15]
        # holds "next instruction" (pc+2 in Thumb, pc+8 in ARM) which is a
        # different convention from Unicorn's architectural PC, so it would
        # never match. A control-flow divergence is caught by the first field
        # of the following line instead, which is the instruction's own PC.
        return [uc.reg_read(UC_ARM_REG_R0 + i) for i in range(13)] + [
            uc.reg_read(UC_ARM_REG_R13),
            uc.reg_read(UC_ARM_REG_R14),
        ]

    def on_code(uc_, address, size, user):
        # Unicorn reports an address that may carry the Thumb bit; the C side
        # records the plain address, so normalise.
        pc = address & ~1
        prev = state["prev"]
        if prev is not None:
            r = read_regs()
            out.write("%08x %s %08x\n" % (
                prev, " ".join("%08x" % v for v in r), uc_.reg_read(UC_ARM_REG_CPSR)))

        in_text = TEXT_LO <= pc < TEXT_HI
        in_stub = GUEST_STUB_BASE <= pc < GUEST_STUB_BASE + 0x1000
        if not in_text and not in_stub:
            # Match the interpreter's HALT_BAD_PC, which fires when PC leaves
            # the loaded image. Both endings are real: the return sentinel
            # (LR=1, so a returning guest lands on 0) and a guest that walked
            # into a NULL it was handed by a stubbed import. Without this the
            # oracle would keep executing the zero bytes at address 0 -- the
            # whole space is mapped, so nothing stops it -- and the traces
            # would disagree about where the run ended rather than about what
            # the CPU did.
            state["stopped"] = True
            state["stop_name"] = "PC left the image at %08x" % pc
            uc_.emu_stop()
            return

        if GUEST_STUB_BASE <= pc < GUEST_STUB_BASE + GUEST_STUB_SIZE:
            # A host crossing. The stub address encodes its index, and the dump
            # carries the names, so this can decide what the crossing *is*.
            idx = (pc - GUEST_STUB_BASE) // 4
            nm = import_names[idx] if 0 <= idx < len(import_names) else None

            if nm in REPRODUCIBLE:
                # Both sides perform this one, so the comparison continues
                # through it. Returning to LR the way the interpreter does --
                # T bit from LR's bit 0 -- or the return lands in the wrong
                # instruction set.
                val = do_reproducible(uc_, nm, read_regs())
                if val is not None:
                    uc_.reg_write(UC_ARM_REG_R0, val)
                    lr = uc_.reg_read(UC_ARM_REG_R14)
                    cpsr = uc_.reg_read(UC_ARM_REG_CPSR)
                    cpsr = (cpsr | 0x20) if (lr & 1) else (cpsr & ~0x20)
                    uc_.reg_write(UC_ARM_REG_CPSR, cpsr)
                    # The stub is real guest-visible execution on both sides --
                    # the interpreter emits a line for it at its next loop top --
                    # so record it as the previous instruction and let the next
                    # hook call write it. Without this the traces are one line
                    # out of step after every crossing.
                    state["prev"] = pc
                    uc_.reg_write(UC_ARM_REG_R15, lr & ~1)
                    return

            # Not something both sides can perform: stop here, on both sides,
            # at the same instruction. Naming it is the whole value -- it is the
            # worklist for extending coverage.
            state["stopped"] = True
            state["stop_name"] = nm if nm else "index %d" % idx
            state["stop_name"] = nm
            uc_.emu_stop()
        state["prev"] = pc
        state["count"] += 1

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.reg_write(UC_ARM_REG_R13, GUEST_STACK_TOP - 0x1000)
    uc.reg_write(UC_ARM_REG_R14, RETURN_SENTINEL)

    # A snapshot is applied *after* those defaults, not before: setting SP and
    # LR from it and then overwriting them with the harness's own values leaves
    # the oracle running from a stack the interpreter is not on, which shows up
    # as a register divergence at instruction 0 of a comparison that was
    # supposed to be about the decoder.
    if snap is not None:
        for i in range(13):
            uc.reg_write(UC_ARM_REG_R0 + i, snap[i])
        uc.reg_write(UC_ARM_REG_R13, snap[13])
        uc.reg_write(UC_ARM_REG_R14, snap[14])
        # Only the condition flags come from the snapshot; the mode and T bits
        # belong to Unicorn and writing the interpreter's whole word would clear
        # the Thumb bit and make it decode Thumb code as ARM.
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        uc.reg_write(UC_ARM_REG_CPSR,
                     (cpsr & ~0xF0000000) | (snap[16] & 0xF0000000))
    # Match src/main.c's run_entry(), which starts with the flags clear.
    # Unicorn's power-on CPSR has Z set, so a guest branching on a flag before
    # setting one would diverge for a reason unrelated to the decoder.
    #
    # Only the flags are cleared: the mode and T bits are left exactly as
    # Unicorn set them. Writing a whole literal CPSR here silently clears the
    # Thumb bit and makes Unicorn decode the guest's Thumb code as ARM, which
    # fails on the first push rather than at anything interesting.
    # Only when there is no snapshot: that line exists so a guest branching on
    # a flag before setting one does not diverge for a reason unrelated to the
    # decoder, but with a snapshot the flags are part of the state being
    # compared and clearing them throws away the answer.
    if snap is None:
        uc.reg_write(UC_ARM_REG_CPSR, uc.reg_read(UC_ARM_REG_CPSR) & ~0xF0000000)

    print("oracle: entry=%08x budget=%d" % (entry, budget))
    try:
        uc.emu_start(entry, 0, 0, budget)
    except UcError as e:
        print("oracle: stopped on %s after %d instructions"
              % (e, state["count"]))
    else:
        print("oracle: ran %d instructions" % state["count"])
    if state.get("stop_name"):
        print("oracle: stopped at import %r -- not reproducible on both sides"
              % state["stop_name"])

    # Emit the final instruction's state too, matching nothing on the C side
    # only if it stopped cleanly; the diff will show it as a trailing line.
    out.close()
    print("oracle: trace -> %s" % trace_path)


if __name__ == "__main__":
    main()
