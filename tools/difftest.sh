#!/bin/sh
# Differential-test the interpreter against Unicorn, one guest entry point at
# a time. Run from the repository root: tools/difftest.sh [libgame.so]
#
# Both sides start from the same relocated memory image (the C side dumps it),
# both stub every import to a no-op returning 0, and both stop when PC leaves
# the image. What is left is pure CPU semantics, so a mismatch is a decoder bug
# and difftrace.py names the instruction.
set -e
cd "$(dirname "$0")/.."

SO=${1:-game/libgame.so}
PY=.venv/bin/python3
DUMP=/tmp/inotia3-guest.bin
CT=/tmp/inotia3-interp.trace
OT=/tmp/inotia3-oracle.trace

[ -x build/spike ] || { echo "build first: tools/build.sh native"; exit 1; }
[ -x "$PY" ] || {
    echo "no oracle environment. See docs/SPIKE.md; in short:"
    echo "  python3 -m venv --without-pip .venv"
    echo "  # then unpack the unicorn wheel from PyPI into"
    echo "  # .venv/lib/python*/site-packages"
    exit 1
}

# The four static constructors from .init_array, which run before JNI_OnLoad.
ENTRIES="010b6a09 010d03c5 010d0b3d 010ea589"

fail=0
for e in $ENTRIES; do
    ./build/spike "$SO" --oracle --entry=$e --budget=5000000 \
        --dump-guest=$DUMP --trace-insns=$CT >/dev/null 2>&1 || true
    $PY tools/oracle.py $DUMP $OT $e 5000000 >/dev/null 2>&1 || true
    result=$($PY tools/difftrace.py $CT $OT 2>&1 |
             grep -E "traces are identical|first divergence" | head -1)
    printf '  entry %s : %s\n' "$e" "$result"
    case "$result" in
        *identical*) ;;
        *) fail=1;
           $PY tools/difftrace.py $CT $OT --context=2 | head -20 ;;
    esac
done

[ $fail -eq 0 ] && echo "all entries match" || echo "DIVERGENCE -- see above"
exit $fail
