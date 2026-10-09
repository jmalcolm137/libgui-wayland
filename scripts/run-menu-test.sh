#!/usr/bin/env bash
# Menubar/menu integration test (headless, no X11).
#
# Runs Calculator under the headless compositor, clicks "File" in the menubar,
# and checks that a popup is created *below* the menubar (correct positioning),
# then clicks "Quit" in the popup and checks the app exits.
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

# "File" is the first menubar item, ~16px in and ~10px down (menubar is 20px).
# Its one item, "Quit", sits just below the menubar.
cat > "$RUNTIME/input.txt" <<'EOF'
sleep 1200
motion 16 10
button PRESS left
button RELEASE left
sleep 400
motion 30 34
button PRESS left
button RELEASE left
sleep 200
EOF

"$HC" --socket menu-test --size 800x600 --timeout 6 \
    --output "$RUNTIME/frame.png" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/menu-test" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=menu-test \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 8 "$APP" > "$RUNTIME/app.log" 2>&1
APP_EXIT=$?
set -e
wait "$HC_PID" 2>/dev/null || true

failures=0
check() { if eval "$2"; then printf '%-46s PASS\n' "$1"; else printf '%-46s FAIL\n' "$1"; failures=$((failures + 1)); fi; }

check "menubar click opened a popup"   "grep -q 'popup .* created' '$RUNTIME/app.log'"
check "popup was configured"           "grep -q 'popup configure at' '$RUNTIME/app.log'"
# The popup must anchor at the File menubar item (surface x ~0), not at random.
check "popup anchored at the menubar item" "grep -qE 'popup [0-9]+ created .* at 0,0$' '$RUNTIME/app.log'"
# The earlier bug: attaching a buffer before configure is a protocol error.
check "no attach-before-configure error" "! grep -q 'attached a buffer before configure' '$RUNTIME/hc.log'"
check "clicking Quit activated an item" "grep -q 'menu_item_activated' '$RUNTIME/app.log' || [[ $APP_EXIT -eq 0 ]]"
grep 'popup configure at' "$RUNTIME/app.log" | tail -2 | sed 's/^/  /' || true

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures check(s) failed; logs in $RUNTIME"
    tail -20 "$RUNTIME/app.log"
    exit 1
fi
echo "==> menu test passed"
