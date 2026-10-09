#!/usr/bin/env bash
# Native Wayland clipboard integration test (headless, no X11).
#
# For each representation (text, image/png, text/uri-list):
#   1. A native libwayland-client peer owns the selection.
#   2. libwm-clipboard-test reads it (logging what LibGUI sees) and publishes
#      its own data.
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

# A 4x4 red PNG, generated without external tools.
python3 - "$RUNTIME/red.png" <<'PY'
import struct, sys, zlib
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
w = h = 4
raw = b"".join(b"\x00" + bytes([255, 0, 0]) * w for _ in range(h))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY
printf 'file:///tmp/peer-a\nfile:///tmp/peer-b\n' > "$RUNTIME/uris.txt"

cleanup() {
    kill "$PEER_PID" "$APP_PID" "$HC_PID" 2>/dev/null || true
    if [[ "${KEEP:-0}" == "1" ]]; then
        echo "==> logs kept in $RUNTIME"
    else
        rm -rf "$RUNTIME"
    fi
}
PEER_PID=""; APP_PID=""; HC_PID=""
trap cleanup EXIT

"$HC" --socket clip-test --size 800x600 --timeout 60 --output "$RUNTIME/frame.png" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/clip-test" ]] && break; sleep 0.1; done

failures=0
check_contains() {
    if grep -q "$2" "$3"; then printf '%-46s PASS\n' "$1"; else printf '%-46s FAIL\n' "$1"; failures=$((failures + 1)); fi
}

run_scenario() {
    local name="$1" mode="$2"; shift 2
    local peer_log="$RUNTIME/peer-$name.log" app_log="$RUNTIME/app-$name.log" get_out="$RUNTIME/get-$name.out"

    WAYLAND_DISPLAY=clip-test "$PEER_BIN" "$@" > "$peer_log" 2>&1 &
    PEER_PID=$!
    for _ in $(seq 1 50); do grep -q "serving selection" "$peer_log" 2>/dev/null && break; sleep 0.1; done

    WAYLAND_DISPLAY=clip-test SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
        LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$APP" $mode > "$app_log" 2>&1 &
    APP_PID=$!
    for _ in $(seq 1 50); do grep -q "wrote" "$app_log" 2>/dev/null && break; sleep 0.1; done
    sleep 0.3

    WAYLAND_DISPLAY=clip-test timeout 5 "$PEER_BIN" get "${GET_MIME:-}" > "$get_out" 2>&1 || true
    wait "$APP_PID" 2>/dev/null || true
    kill "$PEER_PID" 2>/dev/null || true
    wait "$PEER_PID" 2>/dev/null || true
}

# --- text --------------------------------------------------------------------
GET_MIME=""
run_scenario text "" set "headless-clip-hello"
check_contains "text: app read the peer's text"      "content='headless-clip-hello'" "$RUNTIME/app-text.log"
check_contains "text: peer read the app's text"      "libwm-clipboard-hello" "$RUNTIME/get-text.out"

# --- image/png <-> image/x-serenityos ----------------------------------------
GET_MIME="image/png"
run_scenario image image set-file image/png "$RUNTIME/red.png"
check_contains "image: app decoded the peer PNG"     "read mime='image/x-serenityos'" "$RUNTIME/app-image.log"
check_contains "image: decoded 4x4"                  "decoded image 4x4" "$RUNTIME/app-image.log"
if [[ "$(head -c 4 "$RUNTIME/get-image.out" | od -An -tx1 | tr -d ' \n')" == "89504e47" ]]; then
    printf '%-46s PASS\n' "image: peer read a PNG from the app"
else
    printf '%-46s FAIL\n' "image: peer read a PNG from the app"; failures=$((failures + 1))
fi

# --- text/uri-list -----------------------------------------------------------
GET_MIME="text/uri-list"
run_scenario uris uri-list set-file text/uri-list "$RUNTIME/uris.txt"
check_contains "uri-list: app read the peer's list"  "read mime='text/uri-list'" "$RUNTIME/app-uris.log"
check_contains "uri-list: peer read the app's list"  "file:///tmp/libwm-a" "$RUNTIME/get-uris.out"

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures check(s) failed"
    exit 1
fi
echo "==> clipboard test passed"
