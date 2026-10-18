#!/bin/sh
# play-linux.sh -- play the port on Linux without thinking about servers.
#
#   tools/play-linux.sh            # build if needed, serve, open the browser
#   PORT=9000 tools/play-linux.sh  # pick the port
#
# The browser is the only host with a complete renderer, a real font stack and
# audio (see docs/HANDOFF.md, "Build and run"): the native build is the
# debugging/measurement host and deliberately has no window. So "playing on
# Linux" is this: the wasm build, served from this directory and opened in the
# default browser. Ctrl-C stops the server.
set -e
cd "$(dirname "$0")/.."

if [ ! -f build/spike.html ] || [ ! -f build/spike.wasm ] || [ ! -f build/spike.data ]; then
    echo "building the browser target first..."
    tools/build.sh browser
fi

PORT=${PORT:-8080}
URL="http://127.0.0.1:$PORT/spike.html"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory build >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT INT TERM
sleep 1

echo "Inotia 3 (wasm port) -> $URL"
echo "  - audio needs one click on the page (browser autoplay policy)"
echo "  - language follows the browser's locale; ?lang=zh-CN forces one"
echo "Close this terminal (Ctrl-C) to stop the server."

if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$URL" >/dev/null 2>&1 || true
elif command -v sensible-browser >/dev/null 2>&1; then
    sensible-browser "$URL" >/dev/null 2>&1 || true
else
    echo "(open the URL above in a browser)"
fi

wait $SRV
