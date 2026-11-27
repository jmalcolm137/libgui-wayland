#!/usr/bin/env bash
# Desktop appearance from the standard Settings portal.
#
# Fonts, pointer/keyboard settings and the display layout have no portable API
# (they stay the desktop's own, over serenity-window), but appearance does:
# org.freedesktop.appearance color-scheme on org.freedesktop.portal.Settings.
# When no desktop sends a theme (no serenity-window host settings), LibWM reads
# it and applies the bundled Dark or Light theme, following SettingChanged.
#
# The test runs a private session bus with tools/fake-settings-portal as the
# portal: first preferring dark, then (after a delay) switching to light. The
# same fake portal answers org.freedesktop.portal.Screenshot with a small PNG,
# which exercises LibWM's capture fallback for the Magnifier, `shot` and the
# colour picker when the compositor has no serenity-window capture.
set -euo pipefail

# The checks below assert on LibWM's trace output (see Environment.h).
export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-test-window"

if ! pkg-config --exists libsystemd; then
    echo "==> no libsystemd; skipping the Settings portal test"
    exit 77
fi
if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-test-window >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-portal.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
mkdir -p "$RUNTIME/session/portal" "$RUNTIME/config"

cc -O2 -Wall -o "$RUNTIME/fake-settings-portal" "$PROJECT_ROOT/tools/fake-settings-portal.c" $(pkg-config --cflags --libs libsystemd)

# A solid 80x60 PNG (#336699) for the Screenshot portal. The portal hands out a
# fresh copy per request because LibWM unlinks what it reads.
base64 -d > "$RUNTIME/shot.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAFAAAAA8CAIAAAB+RarbAAAAeUlEQVR4nOXOQQEAIBCAMKSSnexkW1ucD5Zga59LicRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjMRIjL8D0x77qgGqPEg5hgAAAABJRU5ErkJggg==
PNG

# A private session bus, so the test neither needs nor disturbs the user's.
mapfile -t BUS < <(dbus-daemon --session --fork --print-address=1 --print-pid=1)
export DBUS_SESSION_BUS_ADDRESS="${BUS[0]}"
BUS_PIDS+=("${BUS[1]}")

PORTAL_PID=""
HC_PID=""
cleanup() {
    [[ -n "${HC_PID}" ]] && kill "$HC_PID" 2>/dev/null || true
    [[ -n "${PORTAL_PID}" ]] && kill "$PORTAL_PID" 2>/dev/null || true
    for pid in "${BUS_PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"
}
BUS_PIDS=()
trap cleanup EXIT

"$RUNTIME/fake-settings-portal" --scheme 1 --change-after 1600 --change-to 2 \
    --screenshot "$RUNTIME/shot.png" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
for _ in $(seq 1 100); do
    gdbus call --session --dest org.freedesktop.portal.Desktop \
        --object-path /org/freedesktop/portal/desktop \
        --method org.freedesktop.portal.Settings.Read org.freedesktop.appearance color-scheme >/dev/null 2>&1 && break
    sleep 0.1
done

"$HC" --socket libwm-portal --size 800x600 --timeout 5 \
    --output "$RUNTIME/screen.png" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-portal" ]] && break; sleep 0.1; done

# XDG_CONFIG_HOME points into the runtime directory: no SerenityDE theme broker
# exists, so the portal is the only appearance source.
env XDG_RUNTIME_DIR="$RUNTIME" XDG_CONFIG_HOME="$RUNTIME/config" \
    LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-portal \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 6 "$APP" > "$RUNTIME/app.log" 2>&1 || true

wait "$HC_PID" 2>/dev/null || true
HC_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "portal: dark preference applied"        "grep -q \"system theme is 'Dark' (from the settings portal)\" '$RUNTIME/app.log'"
check "portal: light preference followed live" "grep -q \"system theme is 'Light' (from the settings portal)\" '$RUNTIME/app.log'"
check "portal: appearance change was emitted"  "grep -q 'changed to color-scheme=2' '$RUNTIME/portal.log'"
check "screenshot: whole-screen capture works" "grep -q 'screen_bitmap=true' '$RUNTIME/app.log'"
check "magnifier: around-cursor capture works" "grep -q 'around_cursor=true' '$RUNTIME/app.log'"
check "magnifier: around-location capture works" "grep -q 'around_location=true' '$RUNTIME/app.log'"
check "picker: colour under cursor is read"    "grep -q 'color under cursor #336699' '$RUNTIME/app.log'"
check "screenshot: the portal served an image" "grep -q 'screenshot response file://' '$RUNTIME/portal.log'"
check "screenshot: repeated asks reuse a capture" "test \"\$(grep -c 'screenshot response' '$RUNTIME/portal.log')\" -le 2"
check "portal: no crash or assertion"          "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures portal check(s) failed; logs in $RUNTIME"
    grep -E 'theme|portal|colour' "$RUNTIME/app.log" | tail -10 || tail -20 "$RUNTIME/app.log"
    tail -5 "$RUNTIME/portal.log" 2>/dev/null || true
    exit 1
fi
echo "==> Settings portal test passed"
