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

import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R13, UC_ARM_REG_R14,
    UC_ARM_REG_R15, UC_ARM_REG_CPSR,
)

# The guest is ARMv7-A Thumb-2 (GCC emits movw/movt and the wide ldm/stm forms
# on nearly every function). Unicorn's default ARM CPU is an older core whose
# decoder does not know those encodings, so the oracle raised UC_ERR_INSN_INVALID
# on the first one and the differential had never compared a single instruction.
# Cortex-A7 is the closest model to the target (the phone this shipped for is
# ARMv7-A) and it decodes everything the guest uses.
def cpu_model_setup(uc):
    from unicorn.arm_const import UC_CPU_ARM_CORTEX_A7
    if hasattr(uc, "ctl_set_cpu_model"):       # unicorn 2.x python bindings
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A7)
    else:                                      # unicorn 1.x
        from unicorn import uc_ctl_set_cpu_model
        uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A7)

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
# The executable range of the image. The dump carries the real bounds (TEXT
# block, written by src/main.c) because hardcoding them here goes stale the
# moment the .so is rebuilt; these are only the fallback for a dump written
# before that block existed.
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

    # Trailing blocks, each with its own magic, in whatever order the writer
    # chose. A reader that predates a block skips it by not looking, and this
    # reader does not care where a block sits -- the first version of this
    # function assumed REGS then INAM then TEXT and silently lost the import
    # names when the writer put TEXT in the middle, which turned every crossing
    # into "not reproducible" and quietly shortened every trace.
    regs = None
    names = []
    text = None
    while off + 4 <= len(blob):
        tag = blob[off:off + 4]
        off += 4
        if tag == b"REGS":
            regs = list(struct.unpack_from("<16I", blob, off))
            off += 64
            regs.append(struct.unpack_from("<I", blob, off)[0])   # cpsr
            off += 4
        elif tag == b"INAM":
            ni = struct.unpack_from("<I", blob, off)[0]
            off += 4
            for _ in range(ni):
                ln = struct.unpack_from("<I", blob, off)[0]
                off += 4
                names.append(blob[off:off + ln].decode("latin1"))
                off += ln
        elif tag == b"TEXT":
            text = struct.unpack_from("<II", blob, off)
            off += 8
        else:
            break
    return out, names, regs, text


def do_reproducible(uc, name, r):
    """Perform one of the imports both sides implement, and return the value for
    r0 -- or None if this name is not one of them.

    These must match src/host_libc.c byte for byte. A difference here is a
    divergence the harness caused, which is worse than not running the import at
    all: it would be chased as a decoder bug.

    The shared contract, and it is the same one the shims implement: an
    argument range that leaves the 144 MB address space means the call does
    nothing and returns 0 (or the destination, for the copy forms). The guest
    space is one flat buffer in the interpreter, so `ok()` is a bounds check
    there rather than a fault -- and a sweep that calls every function with
    zeroed arguments produces wild pointers constantly. Both sides must answer
    those the same way, or the same null-pointer class is reported as a decoder
    mismatch on every entry that happens to call strlen with a bad pointer.
    """
    def ok(a, n):
        return n <= GUEST_MEM_SIZE and a <= GUEST_MEM_SIZE - n

    def mem(a, n):
        return bytes(uc.mem_read(a, n))

    if name == "memcpy" or name == "memmove":
        dst, src, n = r[0], r[1], r[2]
        if ok(dst, n) and ok(src, n) and n:
            uc.mem_write(dst, mem(src, n))
        return dst
    if name == "memset":
        dst, c, n = r[0], r[1], r[2]
        if ok(dst, n) and n:
            uc.mem_write(dst, bytes([c & 0xFF]) * n)
        return dst
    if name == "memcmp":
        a, b, n = r[0], r[1], r[2]
        if not n or not ok(a, n) or not ok(b, n):
            return 0
        x, y = mem(a, n), mem(b, n)
        d = 0
        for i in range(n):
            if x[i] != y[i]:
                d = x[i] - y[i]
                break
        return d & 0xFFFFFFFF
    if name in ("strlen", "strcmp", "strncmp", "strcpy", "strncpy"):
        def cstr(a):
            """The guest string at `a`, or None if it has no NUL before the end
            of the address space.

            Bounded by the space, not by a cap: the previous version read byte
            by byte until it faulted, which turned "the entry passed a wild
            pointer to strlen" into a bare UC_ERR_READ_UNMAPPED with no address
            -- the same class of failure the interpreter's shims answer with 0.
            Reading past the end of the space is a failed read, and this returns
            None so the caller can do what src/host_libc.c does. """
            if a >= GUEST_MEM_SIZE:
                return None
            out = bytearray()
            while a + len(out) < GUEST_MEM_SIZE:
                c = uc.mem_read(a + len(out), 1)[0]
                if c == 0:
                    return bytes(out)
                out.append(c)
            return None
        if name == "strlen":
            s = cstr(r[0])
            return 0 if s is None else len(s)
        if name in ("strcmp", "strncmp"):
            # Compare byte by byte up to the NUL or n, whichever comes first,
            # and return 0 if either pointer leaves the space first -- exactly
            # the loop in src/host_libc.c's guest_strcmp(). Comparing whole
            # strings is not the same function: strncmp must not need a NUL
            # anywhere, only the bytes it compares.
            a, b = r[0], r[1]
            n = 0xFFFFFFFF if name == "strcmp" else r[2]
            la = min(n, GUEST_MEM_SIZE - a) if a < GUEST_MEM_SIZE else 0
            lb = min(n, GUEST_MEM_SIZE - b) if b < GUEST_MEM_SIZE else 0
            lim = min(la, lb)
            x = mem(a, lim) if lim else b""
            y = mem(b, lim) if lim else b""
            for i in range(lim):
                if x[i] != y[i]:
                    return (x[i] - y[i]) & 0xFFFFFFFF
                if x[i] == 0:
                    return 0
            return 0
        if name == "strcpy" or name == "strncpy":
            dst, src = r[0], r[1]
            s = cstr(src)
            if s is None:
                return dst
            if name == "strcpy":
                n = len(s) + 1
                if ok(dst, n):
                    uc.mem_write(dst, s + b"\x00")
            else:
                n = r[2]
                body = (s[:n] + b"\x00" * max(0, n - len(s)))[:n]
                if ok(dst, n) and n:
                    uc.mem_write(dst, body)
            return dst
    return None


def run_trace(dump_path, trace_path, entry=None, budget=20_000_000,
              image=None, quiet=False):
    """Run one entry under Unicorn, write the interpreter's trace format.

    `image` is an already-loaded (regions, names, snap) triple. A sweep over
    thousands of entries reads the same 95 MB dump every time otherwise, which
    costs more than the emulation itself.

    Returns a dict describing how the run ended, so a caller can tell "both
    sides stopped at the same import" from "the oracle faulted".
    """
    def say(msg):
        if not quiet:
            print(msg)

    if image is None:
        image = load_dump(dump_path)
    regions, import_names, snap, text = image
    if text is not None:
        global TEXT_LO, TEXT_HI
        TEXT_LO, TEXT_HI = text

    # Only regions that hold something need writing: Unicorn zero-fills what
    # mem_map hands out, and the interpreter's space holds zeros there too (the
    # dump is of the same fresh image). A pre-run dump is 64 MB of heap and
    # 8 MB of stack with not one non-zero byte, and copying those into Unicorn
    # costs more than emulating the entry.
    regions = [(a, d) for a, d in regions if d.count(0) != len(d)]

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu_model_setup(uc)

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
    #
    # An entry named on the command line wins over the snapshot's PC. The C side
    # sets r15 to the entry it is about to run when it dumps a pre-run image,
    # but it has no reason to keep that exact bit: the argument carries the
    # Thumb bit explicitly, and taking it from the argument means an ARM entry
    # (bit 0 clear) is not silently promoted to Thumb, which is what `| 1` on
    # the snapshot does for the resume case where the bit is implicit.
    if snap is not None and entry is None:
        entry = snap[15] | 1
        say("oracle: resuming from a snapshot at %08x" % (entry & ~1))

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
        if os.environ.get("ORACLE_DEBUG"):
            say("hook %08x prev=%s" % (pc, "%08x" % state["prev"]
                                       if state["prev"] is not None else "None"))
        prev = state["prev"]
        if prev is not None:
            r = read_regs()
            out.write("%08x %s %08x\n" % (
                prev, " ".join("%08x" % v for v in r), uc_.reg_read(UC_ARM_REG_CPSR)))

        in_text = TEXT_LO <= pc < TEXT_HI
        in_stub = GUEST_STUB_BASE <= pc < GUEST_STUB_BASE + IMPORT_MAX * 4
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
            # The line for the instruction that jumped here has just been
            # written; clearing `prev` keeps the final flush from writing it a
            # second time. (A *fetch* fault outside the image never reaches this
            # hook at all, and that is the case the flush exists for.)
            state["prev"] = None
            uc_.emu_stop()
            return

        if GUEST_STUB_BASE <= pc < GUEST_STUB_BASE + IMPORT_MAX * 4:
            # A host crossing. The stub address encodes its index, and the dump
            # carries the names, so this can decide what the crossing *is*.
            # The bound is the stub page the C side actually installs
            # (IMPORT_MAX entries of four bytes), not GUEST_STUB_SIZE: a PC
            # between the two is BAD_PC on the interpreter and would be read as
            # a crossing here, which shows up as one trace ending and the other
            # running on -- a harness divergence, not a decoder one.
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
                    # The harness contract for a reproducible import is
                    # (r0 = result, r1 = 0): the interpreter's host_import_call
                    # clears both return registers before running the real
                    # implementation, and these imports return only in r0. An
                    # import is free to clobber r1 under AAPCS, so this is a
                    # choice both sides must make the same way -- not a fact
                    # about ARM. Leaving r1 alone made every import a guaranteed
                    # one-register difference on the very next instruction.
                    uc_.reg_write(UC_ARM_REG_R1, 0)
                    lr = uc_.reg_read(UC_ARM_REG_R14)
                    # Return the way the interpreter does -- through LR, with
                    # its bit 0 selecting the instruction set. Write the PC
                    # *with* that bit rather than masking it and setting the
                    # CPSR T bit: a CPSR write from inside a code hook does not
                    # stick (Unicorn restores it; verified by writing 0x...1f3
                    # and reading back 0x...1d3), so masking the bit executed
                    # the Thumb return address as ARM, decoded the prologue as
                    # an ARM load and faulted on a wild pointer. Writing R15
                    # with bit 0 set is the mode switch Unicorn honours.
                    #
                    # The stub is real guest-visible execution on both sides --
                    # the interpreter emits a line for it at its next loop top --
                    # so record it as the previous instruction and let the next
                    # hook call write it. Without this the traces are one line
                    # out of step after every crossing.
                    state["prev"] = pc
                    uc_.reg_write(UC_ARM_REG_R15, lr)
                    return

            # Not something both sides can perform: stop here, on both sides,
            # at the same instruction. Naming it is the whole value -- it is the
            # worklist for extending coverage.
            #
            # The stub's own line is dropped, not left pending: the interpreter
            # clears g_have_prev on this path too, because the stub is the
            # harness's dispatch mechanism and not guest code the oracle has a
            # counterpart for. cpu_trace_final() then emits nothing, so a flush
            # here would add a line the interpreter never writes.
            state["stopped"] = True
            state["stop_name"] = nm if nm else "index %d" % idx
            state["prev"] = None
            uc_.emu_stop()
            return
        state["prev"] = pc
        state["count"] += 1

    uc.hook_add(UC_HOOK_CODE, on_code)

    # A guest access outside the flat 144 MB space stops the comparison with
    # "Invalid memory write" and no address, which reads like an oracle crash
    # rather than the guest bug it is. The interpreter has no bounds check at
    # all (it indexes one host buffer), so this is also the only place a wild
    # pointer is visible. Name it.
    def on_unmapped(uc_, access, address, size, value, user):
        say("oracle: guest %s at %08x size %d (pc=%08x) -- outside the "
            "guest space, stopping" %
            ("write" if access & 1 else "read", address, size,
             uc_.reg_read(UC_ARM_REG_R15)))
        state["stopped"] = True
        state["stop_name"] = "unmapped access at %08x" % address
        uc_.emu_stop()
        return False

    from unicorn import UC_HOOK_MEM_WRITE_UNMAPPED, UC_HOOK_MEM_READ_UNMAPPED
    uc.hook_add(UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_READ_UNMAPPED,
                on_unmapped)
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

    say("oracle: entry=%08x budget=%d" % (entry, budget))
    error = None
    try:
        uc.emu_start(entry, 0, 0, budget)
    except UcError as e:
        error = str(e)
        say("oracle: stopped on %s after %d instructions"
            % (e, state["count"]))
        if os.environ.get("ORACLE_DEBUG"):
            import traceback
            traceback.print_exc()
    else:
        say("oracle: ran %d instructions" % state["count"])
    if state.get("stop_name"):
        say("oracle: stopped at import %r -- not reproducible on both sides"
            % state["stop_name"])

    # Emit the last retired instruction, matching cpu_trace_final(). The
    # in-loop emit for an instruction happens when the hook for the *next* one
    # fires, so the instruction that ended the run -- a return to the sentinel,
    # a wild access -- would otherwise be missing from the oracle's trace and
    # every early return would look like a length mismatch. The import-stop
    # path above has already cleared state["prev"], because the interpreter
    # discards the stub's line in exactly the same place.
    if state["prev"] is not None:
        r = read_regs()
        out.write("%08x %s %08x\n" % (
            state["prev"], " ".join("%08x" % v for v in r),
            uc.reg_read(UC_ARM_REG_CPSR)))

    out.close()
    say("oracle: trace -> %s" % trace_path)
    return {
        "entry": entry,
        "count": state["count"],
        "stop": state.get("stop_name"),
        "error": error,
    }


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_path, trace_path = sys.argv[1], sys.argv[2]
    entry = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 20_000_000
    info = run_trace(dump_path, trace_path, entry, budget)
    if info["count"] == 0:
        # A run that never started is a harness problem, not a matching trace:
        # say so with a failing exit status rather than report success.
        raise SystemExit("oracle: no instructions ran from entry %08x"
                         % (info["entry"] or 0))


if __name__ == "__main__":
    main()
