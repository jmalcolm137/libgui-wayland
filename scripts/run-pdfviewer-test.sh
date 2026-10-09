#!/usr/bin/env bash
# PDFViewer integration test (headless).
#
# Runs the unmodified SerenityOS PDFViewer under the headless compositor,
# opening a bundled test PDF. This exercises the Config and FileSystemAccess
# portals (provided in-process by LibWM) as well as LibPDF rendering into a
# LibGUI window. The rendered window is dumped to a PNG for inspection.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/PDFViewer"
PDF="${PDF:-$SERENITY_SRC/Tests/LibPDF/complex.pdf}"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/pdfviewer-window.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" PDFViewer >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-pdf.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

printf 'sleep 4000\n' > "$RUNTIME/input.txt"

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-pdf --size 1024x768 --timeout 7 \
    --output "$RUNTIME/hc.png" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!

for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-pdf" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-pdf \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    LIBWM_DUMP="$FRAME_OUT" \
    timeout 8 "$APP" "$PDF" > "$RUNTIME/app.log" 2>&1
APP_EXIT=$?
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

check "Config portal served"          "grep -q \"portal connect request '/tmp/session/0/portal/config'\" '$RUNTIME/app.log'"
check "FileSystemAccess portal served" "grep -q \"portal connect request '/tmp/session/0/portal/filesystemaccess'\" '$RUNTIME/app.log'"
check "PDF Viewer window created"     "grep -q \"title='PDF Viewer'\" '$RUNTIME/app.log'"
check "window presented over wl_shm"  "grep -q 'presented window' '$RUNTIME/app.log'"
check "no fatal verification error"   "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "rendered frame written"        "[[ -s '$FRAME_OUT' ]]"
# A blank window compresses to a tiny PNG; a rendered page is much larger.
check "rendered frame is non-trivial" "[[ \$(stat -c%s '$FRAME_OUT') -gt 3000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures PDFViewer check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> PDFViewer test passed (rendered: $FRAME_OUT)"
