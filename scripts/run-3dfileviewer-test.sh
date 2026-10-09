#!/usr/bin/env bash
# 3DFileViewer integration test (headless, no X11).
#
# Runs the unmodified SerenityOS 3DFileViewer under the headless compositor,
# opening a bundled .obj model. This exercises the *client-side* GL stack:
# LibGL -> LibGPU -> the default backend (EGLGPU over Mesa/EGL, or LibSoftGPU
# when no EGL context is available). GL renders into an offscreen Gfx::Bitmap,
# LibGUI paints that bitmap into the window backing store, and LibWM presents it
# as a wl_shm buffer. The compositor is not involved in rendering at all.
#
# Set LIBGL_GPU_DRIVER=softgpu to force the CPU rasterizer.
#
# The rendered window is dumped to a PNG for inspection.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/3DFileViewer"
MODEL="${MODEL:-$SERENITY_SRC/Base/home/anon/Documents/3D Models/teapot.obj}"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/3dfileviewer-window.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" 3DFileViewer >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-3d.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# Let the render loop run for a few seconds so several frames are drawn, then
# the compositor takes its screenshot.
printf 'sleep 3500\n' > "$RUNTIME/input.txt"

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-3d --size 800x600 --timeout 7 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!

for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-3d" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-3d \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 8 "$APP" "$MODEL" > "$RUNTIME/app.log" 2>&1
set -e

wait "$HC_PID" 2>/dev/null || true

failures=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "3D File Viewer window created"     "grep -q \"title='3D File Viewer'\" '$RUNTIME/app.log'"
check "window presented over wl_shm"      "grep -q 'presented window' '$RUNTIME/app.log'"
check "OBJ model loaded"                  "grep -q 'Wavefront: Loading' '$RUNTIME/app.log'"
check "LibGL rendered the mesh"           "grep -qE 'mesh has [0-9]+ triangles' '$RUNTIME/app.log'"
check "no fatal verification error"       "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "rendered frame written"            "[[ -s '$FRAME_OUT' ]]"
# A blank window (frame + menubar) compresses to a small PNG; a lit, textured
# teapot is substantially larger.
check "rendered frame is non-trivial"     "[[ \$(stat -c%s '$FRAME_OUT') -gt 5000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures 3DFileViewer check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> 3DFileViewer test passed (rendered: $FRAME_OUT)"
