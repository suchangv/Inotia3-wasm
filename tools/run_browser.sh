#!/bin/sh
# Serve build/ and screenshot the page with headless Chrome.
#
# The browser build needs a real WebGL context, which node cannot provide, so
# verification is a headless browser. Chrome for Testing is already on this
# machine (puppeteer's cache); nothing needs installing.
#
#   tools/run_browser.sh [seconds] [out.png]
#
# QUERY=... adds page options to the URL, e.g. QUERY=frames=3000&gltrace=200
set -e
cd "$(dirname "$0")/.."
BUDGET=${1:-20}
OUT=${2:-/tmp/inotia3-frame.png}
PORT=8971

CH=$(find "$HOME/.cache/puppeteer" -type f -name chrome 2>/dev/null | head -1)
[ -x "$CH" ] || { echo "no Chrome for Testing found in ~/.cache/puppeteer"; exit 1; }

[ -f build/spike.html ] || { echo "build first: tools/build.sh browser"; exit 1; }

# The .wasm and .data are fetched, so file:// will not do.
( cd build && python3 -m http.server $PORT >/dev/null 2>&1 & echo $! > /tmp/inotia3-http.pid )
sleep 2
trap 'kill "$(cat /tmp/inotia3-http.pid)" 2>/dev/null || true' EXIT

"$CH" --headless --no-sandbox --disable-gpu --enable-unsafe-swiftshader \
      --hide-scrollbars --window-size=1100,700 \
      --virtual-time-budget=$((BUDGET * 1000)) \
      --enable-logging=stderr --screenshot="$OUT" \
      "http://127.0.0.1:$PORT/spike.html${QUERY:+?$QUERY}" 2>&1 |
    grep -E "CONSOLE|ERROR: 0:" | sed 's/.*CONSOLE:[0-9]*\] //' || true

echo "screenshot -> $OUT"
