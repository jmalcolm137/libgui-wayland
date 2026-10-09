#!/usr/bin/env bash
# Menubar/menu integration test (headless, no X11).
#
# Runs Calculator under the headless compositor and exercises two menus:
#   * File -> Quit  : the item activates and the app exits 0.
#   * Help -> About : the item activates and a second window (the dialog) opens.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/Calculator"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" Calculator >/dev/null

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
check "Help: app stayed running (dialog)"         "[[ $CASE_EXIT -ne 0 ]]"

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

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures check(s) failed; logs in $RUNTIME"
    exit 1
fi
echo "==> menu test passed"
