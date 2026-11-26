#!/usr/bin/env bash
# PDF rendering through LibWM, with LibWM's own PDF test app. The full
# PDFViewer application moved to the desktop, so this suite covers the parts
# that belong to the framework: the FileSystemAccess portal, LibPDF rendering
# into a LibGUI window, presentation over wl_shm, zoom, wheel scrolling and a
# compositor resize.
set -euo pipefail

export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-pdf-test"
PDF="${PDF:-$SERENITY_SRC/Tests/LibPDF/complex.pdf}"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/pdf-test-window.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-pdf-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-pdf.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'kill "$HC_PID" 2>/dev/null || true; [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"' EXIT

# Ctrl+= (KEY_LEFTCTRL 29, KEY_EQUAL 13), twice; then a wheel notch and a resize.
cat > "$RUNTIME/input.txt" <<'EOF'
sleep 1500
motion 320 200
button PRESS left
button RELEASE left
sleep 400
key PRESS 29
key PRESS 13
key RELEASE 13
key RELEASE 29
sleep 250
key PRESS 29
key PRESS 13
key RELEASE 13
key RELEASE 29
sleep 700
motion 320 300
axis vertical 1
sleep 500
resize 800 600
sleep 1800
EOF

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-pdf --size 640x480 --timeout 7 \
    --output "$RUNTIME/frame.png" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
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

check "FileSystemAccess portal served"        "grep -q \"portal connect request '.*/portal/filesystemaccess'\" '$RUNTIME/app.log'"
check "PDF opened through the portal"         "grep -q 'PDF-TEST: opened' '$RUNTIME/app.log'"
check "page rendered with LibPDF"             "grep -q 'PDF-TEST: rendered' '$RUNTIME/app.log'"
check "Ctrl+= re-rendered at a larger size"   "[ \$(grep -c 'PDF-TEST: zoom' '$RUNTIME/app.log') -ge 2 ] && grep -q 'PDF-TEST: rendered 8[0-9][0-9]x1[0-9][0-9][0-9]' '$RUNTIME/app.log'"
check "wheel scrolled the view"               "grep -q 'PDF-TEST: scrolled to [1-9]' '$RUNTIME/app.log'"
check "compositor resize reached the app"     "grep -q 'PDF-TEST: resized 800x' '$RUNTIME/app.log'"
check "window presented over wl_shm"          "grep -q 'presented window' '$RUNTIME/app.log'"
check "no fatal verification error"           "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "rendered frame written"                "[[ -s '$FRAME_OUT' ]]"
check "rendered frame is non-trivial"         "[[ \$(stat -c%s '$FRAME_OUT') -gt 3000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures PDF check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> PDF test passed (rendered: $FRAME_OUT)"
