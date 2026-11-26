#!/usr/bin/env bash
# Menubar/menu integration test (headless, no X11).
#
# Runs LibWM's menubar test app under the headless compositor and exercises two menus:
#   * File -> Quit  : the item activates and the app exits 0.
#   * Help -> About : the item activates and a second window (the dialog) opens.
set -euo pipefail

# The checks below assert on LibWM's trace output (see Environment.h).
export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-menu-test"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-menu-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-menu.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
cleanup() {
    kill "$HC_PID" "$APP_PID" 2>/dev/null || true
    [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"
}
trap cleanup EXIT
HC_PID=""; APP_PID=""

# run_case NAME MENUBAR_X ITEM_Y TIMEOUT
# Clicks menubar item at (MENUBAR_X,10), then the popup item at (30,ITEM_Y).
run_case() {
    local name="$1" menubar_x="$2" item_y="$3" timeout_s="$4"
    cat > "$RUNTIME/$name.input" <<EOF
sleep 1200
motion $menubar_x 10
button PRESS left
button RELEASE left
sleep 500
motion 30 $item_y
button PRESS left
button RELEASE left
sleep 1200
EOF
    "$HC" --socket "menu-$name" --size 800x600 --timeout 6 \
        --output "$RUNTIME/$name.png" --input "$RUNTIME/$name.input" > "$RUNTIME/$name.hc.log" 2>&1 &
    HC_PID=$!
    for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-$name" ]] && break; sleep 0.1; done
    set +e
    env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="menu-$name" \
        SERENITY_ROOT= SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
        LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        timeout "$timeout_s" "$APP" > "$RUNTIME/$name.app.log" 2>&1
    CASE_EXIT=$?
    set -e
    wait "$HC_PID" 2>/dev/null || true
    HC_PID=""
}

failures=0
check() { if eval "$2"; then printf '%-50s PASS\n' "$1"; else printf '%-50s FAIL\n' "$1"; failures=$((failures + 1)); fi; }

# File (x~16) -> Quit (first item, y~13).
run_case quit 16 13 8
check "File: menubar click opened a popup"        "grep -q 'popup .* created' '$RUNTIME/quit.app.log'"
check "File: popup anchored at the item (x=0)"    "grep -qE 'popup [0-9]+ created .* at 0,0( |$)' '$RUNTIME/quit.app.log'"
check "File: no attach-before-configure error"    "! grep -q 'attached a buffer before configure' '$RUNTIME/quit.hc.log'"
check "File: Quit activated an item"              "grep -q 'menu item activated' '$RUNTIME/quit.app.log'"
check "File: app exited after Quit"               "[[ $CASE_EXIT -eq 0 ]]"

# Help (x~225) -> About (third item, y~57).
run_case about 225 57 6
check "Help: menubar click opened a popup"        "grep -q 'popup .* created' '$RUNTIME/about.app.log'"
check "Help: About activated an item"             "grep -q 'menu item activated' '$RUNTIME/about.app.log'"
check "Help: About opened a second window"        "[[ \$(grep -c 'create_window' '$RUNTIME/about.app.log') -ge 2 ]]"
# The app runs until the compositor's timeout closes the connection; a dialog
# left open must not have quit it early (a crash would also be non-zero).
check "Help: app stayed until the compositor closed" "grep -q 'the compositor closed the connection' '$RUNTIME/about.app.log' && [[ $CASE_EXIT -eq 0 || $CASE_EXIT -eq 124 ]]"

# Keyboard navigation: F10 opens the first menu, Down selects Quit, Enter runs it.
cat > "$RUNTIME/kbd.input" <<'EOF'
sleep 1200
key PRESS 68
key RELEASE 68
sleep 400
key PRESS 108
key RELEASE 108
sleep 400
key PRESS 28
key RELEASE 28
sleep 800
EOF
"$HC" --socket menu-kbd --size 800x600 --timeout 6 \
    --output "$RUNTIME/kbd.png" --input "$RUNTIME/kbd.input" > "$RUNTIME/kbd.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-kbd" ]] && break; sleep 0.1; done
set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="menu-kbd" \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 7 "$APP" > "$RUNTIME/kbd.app.log" 2>&1
KBD_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true
HC_PID=""
check "keyboard: F10 opened a menu"               "grep -q 'popup .* created' '$RUNTIME/kbd.app.log'"
check "keyboard: Enter activated an item"         "grep -q 'menu item activated' '$RUNTIME/kbd.app.log'"
check "keyboard: app exited after activation"     "[[ $KBD_EXIT -eq 0 ]]"

# Nested submenu: F10 opens File, Down selects the "New" submenu item, Right
# opens the submenu as a child popup, Enter activates its first item.
ninja -C "$BUILD_DIR" libwm-submenu-test >/dev/null
cat > "$RUNTIME/sub.input" <<'EOF'
sleep 1200
key PRESS 68
key RELEASE 68
sleep 300
key PRESS 108
key RELEASE 108
sleep 300
key PRESS 106
key RELEASE 106
sleep 300
key PRESS 28
key RELEASE 28
sleep 1200
EOF
"$HC" --socket menu-sub --size 800x600 --timeout 8 \
    --output "$RUNTIME/sub.png" --input "$RUNTIME/sub.input" > "$RUNTIME/sub.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-sub" ]] && break; sleep 0.1; done
set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="menu-sub" \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 10 "$BUILD_DIR/bin/libwm-submenu-test" > "$RUNTIME/sub.app.log" 2>&1
SUB_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true
HC_PID=""
check "submenu: F10 opened the root menu"         "grep -q 'popup .* created' '$RUNTIME/sub.app.log'"
check "submenu: child popup was created"          "grep -q 'submenu=true' '$RUNTIME/sub.app.log'"
check "submenu: item activated"                   "grep -q 'SUBMENU-TEST: activated Project' '$RUNTIME/sub.app.log'"
check "submenu: app exited after activation"      "[[ $SUB_EXIT -eq 0 ]]"

# About dialog regression: run it directly so we exercise the GML bitmap load
# (which hardcodes /res/... and used to abort before the path redirect).
ABOUT_APP="$BUILD_DIR/bin/libwm-about-test"
ninja -C "$BUILD_DIR" libwm-about-test >/dev/null
"$HC" --socket menu-aboutdlg --size 800x600 --timeout 5 \
    --output "$RUNTIME/aboutdlg.png" > "$RUNTIME/aboutdlg.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-aboutdlg" ]] && break; sleep 0.1; done
set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="menu-aboutdlg" \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 6 "$ABOUT_APP" > "$RUNTIME/aboutdlg.app.log" 2>&1
ABOUT_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true
HC_PID=""
check "About: widget built (/res redirect works)" "grep -q 'AboutDialogWidget::try_create ok' '$RUNTIME/aboutdlg.app.log'"
check "About: dialog window created"              "[[ \$(grep -c 'create_window' '$RUNTIME/aboutdlg.app.log') -ge 2 ]]"
check "About: did not crash"                      "[[ $ABOUT_EXIT -ne 132 && $ABOUT_EXIT -ne 134 ]]"

# --- Menubar pixels --------------------------------------------------------
# The menubar is rendered on the LibWM server thread and handed to the Wayland
# side as an image; check it is actually drawn (text in the top strip, its
# background across the full width), at 1x, at 2x, and after a resize, which
# must re-render it at the new width.
menubar_frame() {
    local name="$1" scale="$2" input="$3" app="${4:-$APP}"
    printf '%b' "$input" > "$RUNTIME/$name.input"
    "$HC" --socket "menu-$name" --size 640x480 --timeout 3 \
        --output "$RUNTIME/$name.png" --input "$RUNTIME/$name.input" > "$RUNTIME/$name.hc.log" 2>&1 &
    HC_PID=$!
    for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-$name" ]] && break; sleep 0.1; done
    env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="menu-$name" LIBWM_CLIENT_SCALE="$scale" \
        SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
        LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        timeout 4 "$app" > "$RUNTIME/$name.app.log" 2>&1 || true
    wait "$HC_PID" 2>/dev/null || true
    HC_PID=""
}
menubar_pixels_ok() {
    # menubar_pixels_ok NAME SCALE [MIN_WIDTH] [MIN_DARK_PER_SCALE2]
    PROJECT_ROOT="$PROJECT_ROOT" python3 - "$RUNTIME/$1.png" "$2" "${3:-0}" "${4:-200}" <<'PY'
import os, sys
sys.path.insert(0, os.path.join(os.environ["PROJECT_ROOT"], "scripts", "lib"))
from png import read_png_rgba, pixel_rgb
w, h, rows = read_png_rgba(sys.argv[1])
scale, min_width, min_dark = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
strip = 20 * scale
dark = sum(1 for y in range(strip) for x in range(w) if max(pixel_rgb(rows, x, y)) < 80)
right_bg = pixel_rgb(rows, w - 3 * scale, 2 * scale)
ok = w >= min_width and dark >= min_dark * scale * scale and min(right_bg) > 150
print(f"{sys.argv[1]}: {w}x{h} dark={dark} right_bg={right_bg} -> {'ok' if ok else 'BAD'}")
sys.exit(0 if ok else 1)
PY
}
menubar_frame mb1x 1 'sleep 1500\n'
menubar_frame mb2x 2 'sleep 1500\n'
# The menu test window is not resizable; the submenu test window (300x200) is.
ninja -C "$BUILD_DIR" libwm-submenu-test >/dev/null
menubar_frame mbresize 2 'sleep 1000\nresize 600 400\nsleep 1200\n' "$BUILD_DIR/bin/libwm-submenu-test"
check "menubar drawn at 1x"                         "menubar_pixels_ok mb1x 1 >> '$RUNTIME/menubar-pixels.log'"
check "menubar drawn at 2x"                         "menubar_pixels_ok mb2x 2 >> '$RUNTIME/menubar-pixels.log'"
check "menubar re-rendered at new width on resize"  "menubar_pixels_ok mbresize 2 1100 40 >> '$RUNTIME/menubar-pixels.log'"

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures check(s) failed; logs in $RUNTIME"
    exit 1
fi
echo "==> menu test passed"
