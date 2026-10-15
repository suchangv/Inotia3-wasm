#!/usr/bin/env python3
"""Report the first instruction the interpreter and the oracle disagree on.

Both traces are one line per retired instruction: PC, r0-r15, CPSR. The first
differing line is the culprit -- everything before it matched, so the fault is
in that instruction's semantics, not in some earlier state corruption. That is
the whole value: it converts "the guest crashed at 0x01058aa8 after 345
instructions" into "this one opcode computed the wrong register".

Usage: tools/difftrace.py <interp.trace> <oracle.trace> [--context N]
"""

import sys

REGNAMES = ["r%d" % i for i in range(13)] + ["sp", "lr", "pc", "cpsr"]


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

    n = min(len(a), len(b))
    for i in range(n):
        if a[i] == b[i]:
            continue

        # The PCs differing means control flow diverged, which is a different
        # failure from a computed value differing with the PC in step.
        pc_a, pc_b = a[i][0], b[i][0]
        print("\n*** first divergence at instruction %d ***" % i)
        if pc_a != pc_b:
            print("    control flow diverged: the interpreter ran %s, the "
                  "oracle ran %s" % (pc_a, pc_b))
            print("    -> look at instruction %d, whose next-PC the "
                  "interpreter got wrong" % (i - 1))
        else:
            print("    same PC (%s) but different register state" % pc_a)

        for j in range(max(0, i - ctx), min(n, i + 1)):
            mark = ">>" if j == i else "  "
            print("  %s [%d] %s" % (mark, j, " ".join(a[j])))
        print("  oracle [%d] %s" % (i, " ".join(b[i])))

        print("\n    register differences at that instruction:")
        width = max(len(a[i][k]) for k in range(len(a[i])))
        for k in range(1, min(len(a[i]), len(b[i]))):
            if a[i][k] != b[i][k]:
                name = REGNAMES[k - 1] if k - 1 < len(REGNAMES) else "?%d" % k
                print("      %-5s interpreter=%-*s oracle=%s"
                      % (name, width, a[i][k], b[i][k]))
        return 0

    if len(a) != len(b):
        print("\ntraces match for %d instructions, then one ended: "
              "interpreter %d, oracle %d" % (n, len(a), len(b)))
        return 1
    print("\ntraces are identical for all %d instructions" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
