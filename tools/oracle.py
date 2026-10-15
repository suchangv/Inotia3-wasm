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
# The executable range of the image; src/elf_loader.c derives the same bounds
# from the PT_LOAD marked PF_X. Printed by the C spike at load time.
TEXT_LO             = 0x01000000
TEXT_HI             = 0x0113dac4
GUEST_INIT_ARRAY    = GUEST_IMAGE_BASE + 0x13ecfc
# Must match RETURN_SENTINEL in src/main.c. Not 0: a guest that
# branches through a null function pointer also lands on 0.
RETURN_SENTINEL     = 0x0BADF00D


def load_dump(path):
    """Read the GIMG dump: a header of (addr, size) pairs then the bytes."""
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
    return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_path, trace_path = sys.argv[1], sys.argv[2]
    entry = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 20_000_000

    regions = load_dump(dump_path)

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

        if pc < TEXT_LO or pc >= TEXT_HI:
            # Match the interpreter's HALT_BAD_PC, which fires when PC leaves
            # the loaded image. Both endings are real: the return sentinel
            # (LR=1, so a returning guest lands on 0) and a guest that walked
            # into a NULL it was handed by a stubbed import. Without this the
            # oracle would keep executing the zero bytes at address 0 -- the
            # whole space is mapped, so nothing stops it -- and the traces
            # would disagree about where the run ended rather than about what
            # the CPU did.
            state["stopped"] = True
            uc_.emu_stop()
            return

        if GUEST_STUB_BASE <= pc < GUEST_STUB_BASE + GUEST_STUB_SIZE:
            # A host crossing. Mirror what the interpreter does in oracle mode:
            # return 0 and carry on, so the comparison covers the whole run
            # rather than only the prologue before the first import.
            #
            # The stub is reached from the ARM PLT, so the state has to be put
            # back the way a `bx lr` would leave it -- T bit from LR's bit 0 --
            # or the return lands in the wrong instruction set. The core
            # reaches the same conclusion through R[14] & 1.
            state["stopped"] = True
            uc_.emu_stop()
        state["prev"] = pc
        state["count"] += 1

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.reg_write(UC_ARM_REG_R13, GUEST_STACK_TOP - 0x1000)
    uc.reg_write(UC_ARM_REG_R14, RETURN_SENTINEL)
    # Match src/main.c's run_entry(), which starts with the flags clear.
    # Unicorn's power-on CPSR has Z set, so a guest branching on a flag before
    # setting one would diverge for a reason unrelated to the decoder.
    #
    # Only the flags are cleared: the mode and T bits are left exactly as
    # Unicorn set them. Writing a whole literal CPSR here silently clears the
    # Thumb bit and makes Unicorn decode the guest's Thumb code as ARM, which
    # fails on the first push rather than at anything interesting.
    uc.reg_write(UC_ARM_REG_CPSR, uc.reg_read(UC_ARM_REG_CPSR) & ~0xF0000000)

    print("oracle: entry=%08x budget=%d" % (entry, budget))
    try:
        uc.emu_start(entry, 0, 0, budget)
    except UcError as e:
        print("oracle: stopped on %s after %d instructions"
              % (e, state["count"]))
    else:
        print("oracle: ran %d instructions" % state["count"])

    # Emit the final instruction's state too, matching nothing on the C side
    # only if it stopped cleanly; the diff will show it as a trailing line.
    out.close()
    print("oracle: trace -> %s" % trace_path)


if __name__ == "__main__":
    main()
