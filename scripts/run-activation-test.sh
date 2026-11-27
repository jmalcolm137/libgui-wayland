#!/usr/bin/env bash
# xdg-activation-v1 through LibWM. The headless compositor logs every
# activation request, so this checks both paths:
#   * a window opened from a click requests a token with the click's serial;
#   * a launcher-provided XDG_ACTIVATION_TOKEN activates the first window.
set -euo pipefail

export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/libwm-activation-test"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" libwm-activation-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-activation.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'kill "$HC_PID" 2>/dev/null || true; [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"' EXIT

cat > "$RUNTIME/click.input" <<'CLICKEOF'
sleep 1200
motion 160 120
button PRESS left
button RELEASE left
sleep 1200
CLICKEOF

export XDG_RUNTIME_DIR="$RUNTIME"
HC_TRACE=1 "$HC" --socket libwm-activation --size 800x600 --timeout 3 \
    --output "$RUNTIME/click.png" --input "$RUNTIME/click.input" > "$RUNTIME/click.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-activation" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-activation \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 4 "$APP" > "$RUNTIME/click.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

HC_TRACE=1 "$HC" --socket libwm-activation-env --size 800x600 --timeout 2 \
    --output "$RUNTIME/env.png" > "$RUNTIME/env.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-activation-env" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=libwm-activation-env \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    XDG_ACTIVATION_TOKEN=test-token \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 3 "$APP" > "$RUNTIME/env.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-54s PASS\n' "$1"
    else
        printf '%-54s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "serial: the click opened a second window"    "grep -q 'ACTIVATION-TEST: window B created' '$RUNTIME/click.app.log'"
check "serial: a token was requested"               "grep -q 'requested activation token for window' '$RUNTIME/click.app.log'"
check "serial: the compositor activated the window" "grep -q \"HC: activation token 'hc-token-\" '$RUNTIME/click.hc.log'"
check "env: the token was used for the first window" "grep -q 'activated window .* with XDG_ACTIVATION_TOKEN' '$RUNTIME/env.app.log'"
check "env: the compositor saw the launcher token"   "grep -q \"HC: activation token 'test-token'\" '$RUNTIME/env.hc.log'"
check "activation: no fatal verification error"      "! grep -q 'VERIFICATION FAILED' '$RUNTIME/click.app.log' '$RUNTIME/env.app.log'"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures activation check(s) failed; app logs:"
    cat "$RUNTIME/click.app.log" "$RUNTIME/env.app.log"
    echo "--- compositor ---"
    cat "$RUNTIME/click.hc.log" "$RUNTIME/env.hc.log"
    exit 1
fi
echo "==> activation test passed"
