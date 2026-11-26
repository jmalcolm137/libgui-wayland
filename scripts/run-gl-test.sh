#!/usr/bin/env bash
# Client-side GL integration test (headless, no X11, no app from the desktop).
#
# Runs libwm-gl-test twice: once with the Mesa/EGL backend forced
# (LIBGL_GPU_DRIVER=eglgpu) and once with the CPU rasterizer (softgpu). Each run
# renders a solid colour with LibGL, paints the bitmap in a LibGUI window and
# presents it over wl_shm; the window dump is checked for that colour, so a
# broken backend, LibGL, LibGUI or LibWM path fails the test rather than merely
# exiting.
#
# The eglgpu run must not fall back to LibSoftGPU (its "Mesa/EGL unavailable"
# message): with no default EGL display it uses Mesa's surfaceless platform,
# which is what makes headless CI exercise the real GPU backend.
set -euo pipefail

# The checks below assert on LibWM's trace output (see Environment.h).
export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-gl-test"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-gl-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-gl.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"

failures=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

# gl_run DRIVER: render once with that GPU backend, keeping the window dump.
gl_run() {
    local driver="$1"
    printf 'sleep 2500\n' > "$RUNTIME/$driver.input"
    "$HC" --socket "gl-$driver" --size 320x240 --timeout 4 \
        --output "$RUNTIME/$driver.screen.png" --input "$RUNTIME/$driver.input" > "$RUNTIME/$driver.hc.log" 2>&1 &
    local hc_pid=$!
    for _ in $(seq 1 100); do [[ -e "$RUNTIME/gl-$driver" ]] && break; sleep 0.1; done
    env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" \
        LIBWM_TRACE=1 LIBGL_GPU_DRIVER="$driver" \
        WAYLAND_DISPLAY="gl-$driver" \
        SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
        LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        LIBWM_DUMP="$RUNTIME/$driver.png" \
        timeout 5 "$APP" > "$RUNTIME/$driver.app.log" 2>&1 || true
    wait "$hc_pid" 2>/dev/null || true
}

gl_run eglgpu
gl_run softgpu

PROJECT_ROOT="$PROJECT_ROOT" python3 - "$RUNTIME/eglgpu.png" "$RUNTIME/softgpu.png" <<'PY' > "$RUNTIME/pixels.log" 2>&1 || true
import os, sys
sys.path.insert(0, os.path.join(os.environ["PROJECT_ROOT"], "scripts", "lib"))
from png import read_png_rgba, pixel_rgb

expected = (64, 128, 191)  # glClearColor(0.25, 0.5, 0.75, 1)
for path, name in ((sys.argv[1], "eglgpu"), (sys.argv[2], "softgpu")):
    try:
        w, h, rows = read_png_rgba(path)
    except Exception as error:
        print(f"{name}: BAD (no dump: {error})")
        continue
    r, g, b = pixel_rgb(rows, w // 2, h // 2)
    ok = abs(r - expected[0]) <= 3 and abs(g - expected[1]) <= 3 and abs(b - expected[2]) <= 3
    print(f"{name}: {'ok' if ok else 'BAD'} ({w}x{h} centre={r},{g},{b})")
PY

check "eglgpu: LibGL rendered through the EGL backend" "grep -q '^eglgpu: ok' '$RUNTIME/pixels.log'"
check "eglgpu: no silent fallback to LibSoftGPU"       "! grep -q 'falling back to LibSoftGPU' '$RUNTIME/eglgpu.app.log'"
check "softgpu: LibGL rendered through the CPU backend" "grep -q '^softgpu: ok' '$RUNTIME/pixels.log'"
check "gl: both runs reported their render"             "[ \$(grep -h 'GL-TEST: rendered 64x64' '$RUNTIME/eglgpu.app.log' '$RUNTIME/softgpu.app.log' | wc -l) -eq 2 ]"
check "gl: no crash or assertion"                       "! grep -q 'VERIFICATION FAILED' '$RUNTIME/eglgpu.app.log' '$RUNTIME/softgpu.app.log'"

if grep -q 'captured' "$RUNTIME/eglgpu.hc.log"; then
    grep 'captured' "$RUNTIME/eglgpu.hc.log" | tail -1
fi

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures GL check(s) failed; logs in $RUNTIME"
    for f in pixels.log eglgpu.app.log softgpu.app.log; do
        echo "--- $f ---"
        tail -15 "$RUNTIME/$f" 2>/dev/null || true
    done
    exit 1
fi
echo "==> GL test passed"
