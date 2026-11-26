#!/usr/bin/env bash
# Client-side 3D GL through LibWM, with LibWM's own test app. The 3DFileViewer
# application moved to the desktop; this covers the framework half: LibGL renders
# an animated scene into an offscreen bitmap, LibGUI paints it, LibWM presents it.
set -euo pipefail

export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-3d-test"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/3d-test-window.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-3d-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-3d.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'kill "$HC_PID" 2>/dev/null || true; [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"' EXIT

printf 'sleep 2500\n' > "$RUNTIME/input.txt"
export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-3d --size 800x600 --timeout 5 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-3d" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-3d \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 6 "$APP" > "$RUNTIME/app.log" 2>&1
APP_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-50s PASS\n' "$1"
    else
        printf '%-50s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "LibGL rendered frames"                 "[ \$(grep -c '3D-TEST: frame' '$RUNTIME/app.log') -ge 2 ]"
check "window presented over wl_shm"          "grep -q 'presented window' '$RUNTIME/app.log'"
check "no fatal verification error"           "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "rendered frame written"                "[[ -s '$FRAME_OUT' ]]"
check "rendered frame is non-trivial"         "[[ \$(stat -c%s '$FRAME_OUT') -gt 3000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures 3D check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> 3D test passed (rendered: $FRAME_OUT)"
