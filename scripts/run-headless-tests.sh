#!/usr/bin/env bash
# Headless integration test: run a LibGUI app under the pure-Wayland
# headless-compositor, inject a synthetic click, and assert the event reached
# the widget. No X11, no real session.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="${APP:-$BUILD_DIR/bin/libwm-test-window}"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/headless-frame.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-test-window >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-headless.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

cat > "$RUNTIME/input.txt" <<'EOF'
sleep 1500
motion 160 120
button PRESS left
button RELEASE left
EOF

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-test --size 800x600 --timeout 5 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!

for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-test" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-test \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 8 "$APP" > "$RUNTIME/app.log" 2>&1 || true

wait "$HC_PID" 2>/dev/null || true

failures=0
check() {
    if eval "$2"; then
        printf '%-40s PASS\n' "$1"
    else
        printf '%-40s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "window presented over wl_shm"   "grep -q 'presented window' '$RUNTIME/app.log'"
check "pointer entered the window"     "grep -q 'pointer entered window' '$RUNTIME/app.log'"
check "synthetic click reached widget" "grep -q 'TestWindow: mouse down' '$RUNTIME/app.log'"
check "frame captured to PNG"          "[[ -s '$FRAME_OUT' ]]"

if grep -q 'captured' "$RUNTIME/hc.log"; then
    grep 'captured' "$RUNTIME/hc.log" | tail -1
fi

if [[ $failures -ne 0 ]]; then
    echo "==> $failures headless check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> headless input test passed (frame: $FRAME_OUT)"
