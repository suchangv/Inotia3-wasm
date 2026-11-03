#!/bin/sh
# aotdiff.sh -- the correctness gate and the timing A/B for the pre-decoded
# runner (src/aot_predec.c, planned as docs/aot-plan.md M1 step 0).
#
#   tools/aotdiff.sh <guest.so> <symbol|0xENTRY> [budget] [runs]
#
# It runs the same entry twice -- once through the interpreter (--aot=off) and
# once through the pre-decoder (--aot=predec) -- with a per-instruction trace,
# and compares the two files byte for byte. The trace carries pc, r0-r14 and
# cpsr after every retired instruction (host_trace_insn), so "identical" means
# identical machine state at every instruction, not just the same answer.
#
# Then it times both, pinned to one CPU when taskset is available: the numbers
# are only comparable if the process is not migrating between P- and E-cores.
#
# Exit status: 0 when the traces match, 1 when they do not (the first differing
# line is printed), 2 on a usage or build error.
set -e
cd "$(dirname "$0")/.."

SO=${1}
SYM=${2}
BUDGET=${3:-900000000}
RUNS=${4:-5}
TRACE_BUDGET=${TRACE_BUDGET:-2000000}
SPIKE=${SPIKE:-build/spike}

[ -n "$SO" ] && [ -n "$SYM" ] || {
    echo "usage: $0 <guest.so> <symbol|0xENTRY> [budget] [runs]" >&2
    exit 2
}
[ -x "$SPIKE" ] || { echo "$SPIKE missing: run tools/build.sh native" >&2; exit 2; }
command -v node >/dev/null 2>&1 || { echo "node is needed for aot-sym.mjs" >&2; exit 2; }

case "$SYM" in
    0x*|0X*) ENTRY=$SYM ;;
    *)       ENTRY=$(node tools/aot-sym.mjs "$SO" "$SYM") ;;
esac

PIN=""
command -v taskset >/dev/null 2>&1 && PIN="taskset -c 4"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "== same code, two runners: $SO entry=$ENTRY =="
echo "   trace comparison budget=$TRACE_BUDGET; timing budget=$BUDGET"
$PIN "$SPIKE" "$SO" --entry="${ENTRY#0x}" --budget="$TRACE_BUDGET" --no-profile \
      --aot=off --trace-insns="$tmp/off.txt" > "$tmp/off.log" 2>&1
$PIN "$SPIKE" "$SO" --entry="${ENTRY#0x}" --budget="$TRACE_BUDGET" --no-profile \
      --aot=predec --trace-insns="$tmp/on.txt" > "$tmp/on.log" 2>&1

n_off=$(wc -l < "$tmp/off.txt")
n_on=$(wc -l < "$tmp/on.txt")
echo "   traced instructions: interpreter $n_off, predec $n_on"

status=0
if cmp -s "$tmp/off.txt" "$tmp/on.txt"; then
    echo "   traces IDENTICAL (pc, r0-r14, cpsr after every instruction)"
else
    echo "   traces DIFFER -- first divergence:"
    diff "$tmp/off.txt" "$tmp/on.txt" | head -6 | sed 's/^/     /'
    status=1
fi

# The entry line carries the instruction count and the MIPS the runner achieved.
echo "   results at the trace budget:"
grep -h '^entry ' "$tmp/off.log" "$tmp/on.log" | sed 's/^/     /'
grep -h '^aot:' "$tmp/on.log" | sed 's/^/     /'

# The instruction count is the other half of the gate: the trace only covers
# the first TRACE_BUDGET instructions, the count covers the whole run.
full_off=$($PIN "$SPIKE" "$SO" --entry="${ENTRY#0x}" --budget="$BUDGET" \
           --no-profile --aot=off 2>&1 | sed -n 's/^entry *: .* \([0-9]*\) instrs.*/\1/p')
full_on=$($PIN "$SPIKE" "$SO" --entry="${ENTRY#0x}" --budget="$BUDGET" \
          --no-profile --aot=predec 2>&1 | sed -n 's/^entry *: .* \([0-9]*\) instrs.*/\1/p')
echo "   instruction count over the whole run: interpreter $full_off, predec $full_on"
[ "$full_off" = "$full_on" ] || status=1

echo "== timing ($RUNS runs each, per-run MIPS) =="
for mode in off predec; do
    printf "   %-7s" "$mode"
    i=0
    while [ $i -lt "$RUNS" ]; do
        $PIN "$SPIKE" "$SO" --entry="${ENTRY#0x}" --budget="$BUDGET" \
              --no-profile --aot=$mode 2>&1 |
            sed -n 's/.* \([0-9.]*\) MIPS.*/\1/p' | tr -d '\n'
        printf " "
        i=$((i + 1))
    done
    echo
done
echo "   (medians matter; a run this short also measures the turbo state)"

exit $status
