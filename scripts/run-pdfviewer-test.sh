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

# File -> Open: F10 opens the menubar, Down selects Open, Enter opens the
# SerenityOS file picker; type a path into the (focused) filename box and press
# Enter to actually open it. Exercises the picker shown from the server thread.
OPEN_PDF="/tmp/libwm-pdf-open-test.pdf"
cp "$SERENITY_SRC/Tests/LibPDF/colorspaces.pdf" "$OPEN_PDF"

# Emit evdev key events to type a lowercase path (US layout).
type_lowercase_path() {
    local s="$1" out=""
    local i c code
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            a) code=30;; b) code=48;; c) code=46;; d) code=32;; e) code=18;; f) code=33;;
            g) code=34;; h) code=35;; i) code=23;; j) code=36;; k) code=37;; l) code=38;;
            m) code=50;; n) code=49;; o) code=24;; p) code=25;; q) code=16;; r) code=19;;
            s) code=31;; t) code=20;; u) code=22;; v) code=47;; w) code=17;; x) code=45;;
            y) code=21;; z) code=44;; 0) code=11;; 1) code=2;; 2) code=3;; 3) code=4;;
            4) code=5;; 5) code=6;; 6) code=7;; 7) code=8;; 8) code=9;; 9) code=10;;
            -) code=12;; .) code=52;; /) code=53;; *) continue;;
        esac
        out+="key PRESS $code\nkey RELEASE $code\nsleep 50\n"
    done
    printf '%b' "$out"
}
{
    printf 'sleep 2000\nkey PRESS 68\nkey RELEASE 68\nsleep 600\n'
    printf 'key PRESS 108\nkey RELEASE 108\nsleep 400\n'
    printf 'key PRESS 28\nkey RELEASE 28\nsleep 2500\n'
    type_lowercase_path "$OPEN_PDF"
    printf 'sleep 400\nkey PRESS 28\nkey RELEASE 28\nsleep 2500\n'
} > "$RUNTIME/open.input"

"$HC" --socket libwm-pdf-open --size 1280x800 --timeout 12 \
    --output "$RUNTIME/open-hc.png" --input "$RUNTIME/open.input" > "$RUNTIME/open-hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-pdf-open" ]] && break; sleep 0.1; done
set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-pdf-open \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 13 "$APP" > "$RUNTIME/open.app.log" 2>&1
OPEN_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true
rm -f "$OPEN_PDF"

check "picker opened from File -> Open" "grep -q \"title='Open'\" '$RUNTIME/open.app.log'"
check "picker selected and opened a file" "grep -q 'set_window_title.*libwm-pdf-open-test.pdf' '$RUNTIME/open.app.log'"
check "picker flow did not crash"       "! grep -q 'VERIFICATION FAILED' '$RUNTIME/open.app.log'"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures PDFViewer check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    echo "--- picker run log ---"
    cat "$RUNTIME/open.app.log"
    exit 1
fi
echo "==> PDFViewer test passed (rendered: $FRAME_OUT)"
