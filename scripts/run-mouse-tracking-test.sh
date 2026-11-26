#!/usr/bin/env bash
# Global mouse tracking (ext-input-observation-v1) integration test.
#
# The window is at (100,100). The input script moves the pointer to (200,150),
# inside it, then to (1000,750), well outside, and then once more after the
# tracker was destroyed. The shim turns the surface-relative positions the
# compositor reports back into output coordinates with the window's origin, so
# the tracked points are window origin + pointer position either way.
set -euo pipefail

export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-mouse-tracking-test"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/mouse-tracking.frame.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-mouse-tracking-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-track.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'kill "$HC_PID" 2>/dev/null || true; [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"' EXIT

cat > "$RUNTIME/input.txt" <<'EOF'
sleep 800
motion 200 150
sleep 600
motion 1000 750
sleep 1000
motion 700 700
sleep 400
EOF

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-track --size 800x600 --timeout 4 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-track" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-track \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 5 "$APP" > "$RUNTIME/app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-52s PASS\n' "$1"
    else
        printf '%-52s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "window reported its position"            "grep -q 'MOUSE-TRACK-TEST: window at 100,100' '$RUNTIME/app.log'"
check "observer created for the existing window" "grep -q 'pointer observer for window' '$RUNTIME/app.log'"
check "tracking enabled with one observer"      "grep -q 'global mouse tracking enabled (1 observer' '$RUNTIME/app.log'"
check "inside the window: 300,250"              "grep -q 'MOUSETRACK: 300,250' '$RUNTIME/app.log'"
check "outside the window: 1100,850"            "grep -q 'MOUSETRACK: 1100,850' '$RUNTIME/app.log'"
check "one event per cursor movement"           "[ \$(grep -c 'MOUSETRACK:' '$RUNTIME/app.log') -eq 2 ]"
check "tracker destruction disables the observers" "grep -q 'global mouse tracking disabled' '$RUNTIME/app.log'"
check "no positions after disabling"            "! grep -q 'MOUSETRACK: 800,800' '$RUNTIME/app.log'"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures mouse-tracking check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> global mouse tracking test passed (frame: $FRAME_OUT)"
