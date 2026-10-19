#!/usr/bin/env python3
"""Report the first instruction the interpreter and the oracle disagree on.

Both traces are one line per retired instruction: PC, r0-r15, CPSR. The first
differing line is the culprit -- everything before it matched, so the fault is
in that instruction's semantics, not in some earlier state corruption. That is
the whole value: it converts "the guest crashed at 0x01058aa8 after 345
instructions" into "this one opcode computed the wrong register".

Usage: tools/difftrace.py <interp.trace> <oracle.trace> [--context N]

tools/difftest.py imports compare() from here, so a sweep over thousands of
entries uses exactly the rules a human reads, not a second implementation of
them that could be lenient in a different place.
"""

import sys

REGNAMES = ["r%d" % i for i in range(13)] + ["sp", "lr", "cpsr"]


# Only the condition flags are comparable. The interpreter tracks N/Z/C/V and
# nothing else in its CPSR word, while Unicorn reports the full architectural
# register including mode and T bits -- so comparing the word verbatim would
# report a divergence at instruction 0 of every run.
CPSR_FLAGS = 0xF0000000


def read(path):
    out = []
    with open(path) as f:
        for ln in f:
            if not ln.strip():
                continue
            fields = ln.split()
            fields[-1] = "%08x" % (int(fields[-1], 16) & CPSR_FLAGS)
            out.append(fields)
    return out


def compare(a, b, ctx=4):
    """Compare two traces returned by read().

    Returns (status, text). Status is one word so a sweep can summarise
    thousands of entries:
      "identical" -- every retired instruction matched, and both stopped at the
                     same place;
      "diverged"  -- the first differing instruction is named in the text;
      "length"    -- they matched until one ended, which means one side stopped
                     earlier than the other (a trap, a fault, a missed import).
    """
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] == b[i]:
            continue

        # The PCs differing means control flow diverged, which is a different
        # failure from a computed value differing with the PC in step.
        pc_a, pc_b = a[i][0], b[i][0]
        out = ["*** first divergence at instruction %d ***" % i]
        if pc_a != pc_b:
            out.append("    control flow diverged: the interpreter ran %s, the "
                       "oracle ran %s" % (pc_a, pc_b))
            out.append("    -> look at instruction %d, whose next-PC the "
                       "interpreter got wrong" % (i - 1))
        else:
            out.append("    same PC (%s) but different register state" % pc_a)

        for j in range(max(0, i - ctx), min(n, i + 1)):
            mark = ">>" if j == i else "  "
            out.append("  %s [%d] %s" % (mark, j, " ".join(a[j])))
        out.append("  oracle [%d] %s" % (i, " ".join(b[i])))

        out.append("")
        out.append("    register differences at that instruction:")
        width = max(len(a[i][k]) for k in range(len(a[i])))
        for k in range(1, min(len(a[i]), len(b[i]))):
            if a[i][k] != b[i][k]:
                name = REGNAMES[k - 1] if k - 1 < len(REGNAMES) else "?%d" % k
                out.append("      %-5s interpreter=%-*s oracle=%s"
                           % (name, width, a[i][k], b[i][k]))
        return "diverged", "\n".join(out)

    if len(a) != len(b):
        return "length", ("traces match for %d instructions, then one ended: "
                          "interpreter %d, oracle %d" % (n, len(a), len(b)))
    return "identical", "traces are identical for all %d instructions" % n


def signature(a, b):
    """(index, pc, differing register names) of the first divergence, or None.

    A sweep over thousands of entries needs one line per failure that is enough
    to group them: two entries that differ in the same register at the same
    instruction are almost always the same bug.
    """
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] == b[i]:
            continue
        regs = []
        for k in range(1, min(len(a[i]), len(b[i]))):
            if a[i][k] != b[i][k]:
                regs.append(REGNAMES[k - 1] if k - 1 < len(REGNAMES) else "?%d" % k)
        return i, a[i][0], regs
    return None


def compare_files(path_a, path_b, ctx=4):
    return compare(read(path_a), read(path_b), ctx)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ctx = 4
    for a in sys.argv[1:]:
        if a.startswith("--context="):
            ctx = int(a.split("=", 1)[1])
    if len(args) < 2:
        raise SystemExit(__doc__)

    a = read(args[0])
    b = read(args[1])
    print("interpreter: %d instructions" % len(a))
    print("oracle     : %d instructions" % len(b))

    status, text = compare(a, b, ctx)
    print(("\n" if status != "identical" else "") + text)
    return 0 if status == "identical" else 1


if __name__ == "__main__":
    sys.exit(main())
