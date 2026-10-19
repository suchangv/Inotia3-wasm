#!/bin/sh
# Differential-test the interpreter against Unicorn, one guest entry point at
# a time. Run from the repository root: tools/difftest.sh [options] [libgame.so]
#
#   tools/difftest.sh                 the four static constructors (seconds)
#   tools/difftest.sh --all           every FUNC symbol in the image
#   tools/difftest.sh --entry=010b6a09 --entry=...
#   tools/difftest.sh --budget=N --jobs=N --keep
#
# Both sides start from the same relocated memory image (the C side dumps it
# *before* the entry runs), both implement the mem/str imports identically, and
# both stop when PC leaves the image or at the first import neither can perform.
# What is left is pure CPU semantics, so a mismatch is a decoder bug and
# difftrace.py names the instruction. tools/difftest.py does the work; this
# wrapper exists because the docs and the habit both point at .sh.
set -e
cd "$(dirname "$0")/.."

PY=.venv/bin/python3
[ -x build/spike ] || { echo "build first: tools/build.sh native"; exit 1; }
[ -x "$PY" ] || {
    echo "no oracle environment. See docs/SPIKE.md; in short:"
    echo "  python3 -m venv --without-pip .venv"
    echo "  # then unpack the unicorn wheel from PyPI into"
    echo "  # .venv/lib/python*/site-packages"
    exit 1
}
exec "$PY" tools/difftest.py "$@"
