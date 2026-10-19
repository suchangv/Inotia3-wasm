#!/usr/bin/env python3
"""Differential-test the interpreter against Unicorn, entry point by entry point.

Both sides start from the same relocated memory image (the C side dumps it
*before* the entry runs), with the same registers, and both stop when PC leaves
the image or at the first import neither side can perform identically. What is
left is pure CPU semantics, so a mismatch is a decoder bug.

Usage:
    tools/difftest.py                   # the image's static constructors
    tools/difftest.py --all             # every FUNC symbol in the image
    tools/difftest.py --entry=ADDR ...  # named entries (runtime addresses)
    tools/difftest.py --budget=N --jobs=N [libgame.so]

`--all` is the completeness check: it runs every function from its own entry
with zeroed arguments. Most stop after a few dozen instructions at the first
import that cannot be reproduced (malloc, the JNI bridge...), but every one of
those instructions is a decoder check on that function's prologue, which is
where Thumb-2 lives: push.w, movw/movt, ldrd/strd, wide ldm/stm.

Two failure classes are reported apart from a real divergence:

  unmapped  the guest dereferenced an argument that is zero for this entry, so
            it read outside the 144 MB space. The interpreter has no bounds
            check and would silently corrupt its own host buffer; the oracle
            stops. That is the harness calling a function with no arguments,
            not the decoder disagreeing -- but it is worth naming, because a
            function reachable from the game with a null pointer is a crash.

  crash     the interpreter's own process died. Always a bug in this port.
"""

import argparse
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import difftrace
import oracle

# The four static constructors from .init_array. They run before JNI_OnLoad and
# before anything sets up a thread, a heap object or a GL context, so they are
# the one set of entries whose behaviour is not dominated by imports.
CTORS = ["010b6a09", "010d03c5", "010d0b3d", "010ea589"]

IMAGE = None            # (regions, names, snap, text), shared with the workers
SPIKE = os.path.join(ROOT, "build", "spike")
# The bounds-checked build, if it exists: see build_check() in tools/build.sh.
# A sweep calls every function with zero arguments, so the release build takes a
# host SIGSEGV where the oracle stops cleanly, and the two runs are then not
# comparable at all. Prefer the build that stops where the oracle does.
CHECK = os.path.join(ROOT, "build", "spike-check")
# Instructions per entry. Every function that reaches an import stops long
# before this; the cap exists for idle loops (`b .`) and for functions that run
# without crossing at all. It is also the trace size limit: a run that is cut
# off writes 100k lines per side, so raising this for a sweep burns disk.
BUDGET = 100_000
TMP = None


def func_entries(so_path):
    """Every defined FUNC symbol, as a runtime address (Thumb bit included)."""
    out = subprocess.run(["readelf", "-sW", so_path],
                         capture_output=True, text=True, check=True).stdout
    seen = set()
    for ln in out.splitlines():
        f = ln.split()
        if len(f) < 8 or f[3] != "FUNC" or f[6] == "UND":
            continue
        try:
            value = int(f[1], 16)
        except ValueError:
            continue
        if value:
            seen.add(0x01000000 + value)
    return sorted(seen)


def run_one(entry):
    """Run one entry on both sides and compare. Returns a result dict."""
    tag = "%08x" % entry
    ct = os.path.join(TMP, tag + ".interp")
    ot = os.path.join(TMP, tag + ".oracle")

    proc = subprocess.run(
        [SPIKE, SO, "--oracle", "--entry=%s" % tag,
         "--budget=%d" % BUDGET, "--trace-insns=" + ct],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        return {"entry": entry, "status": "crash", "count": 0, "halt": "crash",
                "sig": None, "stop": None,
                "text": "spike exited %d: %s" % (
                    proc.returncode,
                    proc.stderr.decode("utf-8", "replace").strip()[-400:]),
                "paths": (ct, ot)}

    # Why the interpreter stopped, in one word. "budget" is the only reason
    # that makes the traces stop in the middle on purpose -- the interpreter
    # counts a 32-bit Thumb-2 instruction as two (it counts instruction
    # *words*), so a capped run retires about 5% fewer instructions than the
    # oracle's budget, and the two traces end at different lengths without
    # anything being wrong.
    m = re.search(rb"halt=(\S+)", proc.stdout + proc.stderr)
    halt = m.group(1).decode() if m else "?"

    try:
        info = oracle.run_trace(None, ot, entry, BUDGET, image=IMAGE,
                                quiet=True)
    except Exception as e:                       # harness bug, not guest
        return {"entry": entry, "status": "crash", "count": 0, "halt": "crash",
                "sig": None, "stop": None,
                "text": "oracle raised %s: %s" % (type(e).__name__, e),
                "paths": (ct, ot)}

    a = difftrace.read(ct)
    b = difftrace.read(ot)
    status, text = difftrace.compare(a, b, ctx=3)
    if status != "identical" and halt == "budget":
        # The interpreter was still in step when its budget ran out. Compare
        # the common prefix: where each side stops *after* the cap is not a
        # decoder question.
        n = min(len(a), len(b))
        status, text = difftrace.compare(a[:n], b[:n], ctx=3)
        if status == "identical":
            status = "capped"
    if status not in ("identical", "capped") and (
            (info["stop"] or "").startswith("unmapped") or halt == "bad-mem"):
        # The guest walked off with the null argument this entry gave it --
        # `ldr r3, [r3]` with r3 = 0 reads the zero guard and the next one
        # leaves the address space. That is the harness calling a function with
        # no arguments, not the decoder disagreeing: the interpreter (checked
        # build) and the oracle both stop at that instruction, and they differ
        # only in what each leaves in the destination register of the aborted
        # access. Named rather than hidden -- a null dereference reachable from
        # the game is its own bug -- but not counted as a decoder mismatch.
        status = "bad-mem"
        text = "%s; interpreter halt=%s, oracle stopped at %s after %d " \
               "instructions" % (text, halt, info["stop"] or "the end",
                                 info["count"])

    result = {"entry": entry, "status": status, "halt": halt,
              "count": min(len(a), len(b)), "text": text,
              "sig": difftrace.signature(a, b),
              "stop": info["stop"],
              "paths": (ct, ot)}
    if status in ("identical", "capped"):
        os.unlink(ct)
        os.unlink(ot)
    return result


def main():
    global IMAGE, SO, BUDGET, TMP, SPIKE

    ap = argparse.ArgumentParser(
        description="Differential-test the interpreter against Unicorn.")
    ap.add_argument("so", nargs="?", default=os.path.join(ROOT, "game", "libgame.so"))
    ap.add_argument("--all", action="store_true",
                    help="every FUNC symbol instead of the four ctors")
    ap.add_argument("--entry", action="append", default=[],
                    metavar="ADDR", help="runtime address, hex, repeatable")
    ap.add_argument("--budget", type=int, default=BUDGET,
                    help="instructions per entry (default %(default)s)")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    ap.add_argument("--keep", action="store_true",
                    help="keep the trace pairs of every entry")
    ap.add_argument("--max-report", type=int, default=6,
                    help="how many failing entries to print in full")
    ap.add_argument("--log", metavar="FILE",
                    help="write one line per non-identical entry to FILE")
    args = ap.parse_args()

    SO = os.path.abspath(args.so)
    BUDGET = args.budget

    if os.access(CHECK, os.X_OK):
        SPIKE = CHECK
    if not os.access(SPIKE, os.X_OK):
        raise SystemExit("build/spike not built: run tools/build.sh native")

    if args.entry:
        entries = sorted({int(e, 16) for e in args.entry})
    elif args.all:
        entries = func_entries(SO)
    else:
        entries = sorted(int(e, 16) for e in CTORS)
    if not entries:
        raise SystemExit("no entries")

    # Under build/, not /tmp: the traces are the bulk of the cost (a run cut off
    # by the budget writes 100k lines per side), and on this machine /tmp is a
    # small tmpfs while build/ is on the same large filesystem as the image.
    TMP = tempfile.mkdtemp(prefix="difftest-",
                           dir=os.environ.get("DIFFTEST_TMP")
                           or os.path.join(ROOT, "build"))
    dump = os.path.join(TMP, "guest.bin")

    # One dump serves every entry: it is taken before the first entry runs, so
    # it is the loader's image, which is the same for all of them.
    subprocess.run([SPIKE, SO, "--oracle", "--entry=%08x" % entries[0],
                    "--budget=%d" % BUDGET, "--dump-guest=" + dump],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   check=True)
    IMAGE = oracle.load_dump(dump)
    text = IMAGE[3]
    if text:
        lo, hi = text
        before = len(entries)
        entries = [e for e in entries if lo <= (e & ~1) < hi]
        if len(entries) != before:
            print("skipped %d entries outside text %08x-%08x"
                  % (before - len(entries), lo, hi), file=sys.stderr)

    print("difftest: %d entries, budget %d, %d jobs, %s"
          % (len(entries), BUDGET, args.jobs, os.path.basename(SPIKE)),
          file=sys.stderr)

    results = []
    # fork, so the workers share the loaded image copy-on-write instead of each
    # paying for their own copy of the dump.
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(args.jobs) as pool:
        for i, res in enumerate(pool.imap_unordered(run_one, entries, 32), 1):
            results.append(res)
            if i % 500 == 0:
                print("difftest: %d/%d" % (i, len(entries)), file=sys.stderr)

    by = {}
    for r in results:
        by.setdefault(r["status"], []).append(r)
    total_insns = sum(r["count"] for r in results)

    if args.log:
        with open(args.log, "w") as f:
            f.write("# entry status halt insns pc regs oracle-stop\n")
            for r in sorted(results, key=lambda r: (r["status"], r["entry"])):
                if r["status"] in ("identical", "capped"):
                    continue
                sig = r.get("sig")
                f.write("%08x %s %s %d %s %s %s\n" % (
                    r["entry"], r["status"], r["halt"], r["count"],
                    sig[1] if sig else "-", ",".join(sig[2]) if sig else "-",
                    (r.get("stop") or "-").replace(" ", "_")))

    print("\n%d entries, %d instructions compared"
          % (len(results), total_insns))
    for status in ("identical", "capped", "diverged", "length", "bad-mem",
                   "crash"):
        if status in by:
            print("  %-10s %d" % (status, len(by[status])))

    bad = by.get("diverged", []) + by.get("length", []) + by.get("crash", [])

    # Group the failures by where and in what register they diverge. Two entries
    # with the same signature are almost always the same bug, and after a fix
    # this count is what says whether the sweep got better or just moved.
    groups = {}
    for r in bad:
        sig = r.get("sig")
        key = (r["status"], "halt=%s" % r["halt"],
               "pc=%s" % (sig[1] if sig else "-"),
               "regs=%s" % (",".join(sig[2]) if sig else "-"))
        groups[key] = groups.get(key, 0) + 1
    if groups:
        print("\nfailures by signature:")
        for key, n in sorted(groups.items(), key=lambda kv: -kv[1]):
            print("  %4d  %s" % (n, "  ".join(key)))

    for r in bad[:args.max_report]:
        print("\n--- %08x : %s ---" % (r["entry"], r["status"]))
        print(r["text"])
        print("  traces: %s %s" % r["paths"])
    if len(bad) > args.max_report:
        print("\n... and %d more (traces in %s)"
              % (len(bad) - args.max_report, TMP))

    ok = not bad and not by.get("crash")
    if ok:
        if not args.keep:
            shutil.rmtree(TMP, ignore_errors=True)
        print("\nall entries match")
        return 0
    print("\ntraces kept in %s" % TMP)
    return 1


if __name__ == "__main__":
    sys.exit(main())
