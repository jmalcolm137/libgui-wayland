#!/usr/bin/env bash
# Native Wayland clipboard integration test (headless, no X11).
#
#   1. A native libwayland-client peer owns the selection with known text.
#   2. libwm-clipboard-test reads it and publishes its own text.
#   3. The peer reads back what our app published.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
PEER_BIN="${PEER_BIN:-$PROJECT_ROOT/build/wl-clipboard-peer}"
APP="$BUILD_DIR/bin/libwm-clipboard-test"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
cc -O2 -Wall -o "$PEER_BIN" "$PROJECT_ROOT/tools/wl-clipboard-peer.c" $(pkg-config --cflags --libs wayland-client)
ninja -C "$BUILD_DIR" libwm-clipboard-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-clip.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
cleanup() {
    kill "$PEER_PID" "$HC_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

"$HC" --socket clip-test --size 800x600 --timeout 30 --output "$RUNTIME/frame.png" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/clip-test" ]] && break; sleep 0.1; done

WAYLAND_DISPLAY=clip-test "$PEER_BIN" set "headless-clip-hello" > "$RUNTIME/peer.log" 2>&1 &
PEER_PID=$!
for _ in $(seq 1 50); do grep -q "serving selection" "$RUNTIME/peer.log" 2>/dev/null && break; sleep 0.1; done

WAYLAND_DISPLAY=clip-test SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$APP" > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
for _ in $(seq 1 50); do grep -q "wrote" "$RUNTIME/app.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.5

WAYLAND_DISPLAY=clip-test timeout 5 "$PEER_BIN" get > "$RUNTIME/get.log" 2>&1 || true

wait "$APP_PID" 2>/dev/null || true

failures=0
check_contains() {
    if grep -q "$2" "$3"; then
        printf '%-42s PASS\n' "$1"
    else
        printf '%-42s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check_contains "read: app saw the peer's text"   "read mime='text/plain' data='headless-clip-hello'" "$RUNTIME/app.log"
check_contains "write: peer read the app's text" "libwm-clipboard-hello" "$RUNTIME/get.log"

if [[ $failures -ne 0 ]]; then
    echo "==> app log:"; cat "$RUNTIME/app.log"
    echo "==> peer get output: $(cat "$RUNTIME/get.log" 2>/dev/null)"
    exit 1
fi
echo "==> clipboard test passed"
