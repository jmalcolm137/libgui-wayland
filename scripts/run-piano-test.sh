#!/usr/bin/env bash
# Piano integration test (headless).
#
# Runs the unmodified SerenityOS Piano under the headless compositor. This
# exercises LibSerenityAudio: the in-process AudioServer portal, the shared
# ring buffer, the mixer, and (when a PipeWire server is reachable) the
# PipeWire output stream.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/Piano"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/piano-frame.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
ninja -C "$BUILD_DIR" Piano >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-piano.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# PipeWire's socket lives in the real runtime dir, not our sandbox, so point the
# client at it explicitly when one is running.
PW_RUNTIME=""
for candidate in "${PIPEWIRE_RUNTIME_DIR:-}" "/run/user/$(id -u)" "${XDG_RUNTIME_DIR:-}"; do
    if [[ -n "$candidate" && -S "$candidate/pipewire-0" ]]; then
        PW_RUNTIME="$candidate"
        break
    fi
done

printf 'sleep 4000\n' > "$RUNTIME/input.txt"

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-piano --size 1280x900 --timeout 6 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-piano" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" PIPEWIRE_RUNTIME_DIR="$PW_RUNTIME" WAYLAND_DISPLAY=libwm-piano \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    LIBWM_AUDIO_TRACE=1 \
    timeout 7 "$APP" > "$RUNTIME/app.log" 2>&1
set -e
wait "$HC_PID" 2>/dev/null || true

failures=0
skipped=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}
skip() { printf '%-46s SKIP (%s)\n' "$1" "$2"; skipped=$((skipped + 1)); }

check "window presented over wl_shm"       "grep -q 'presented window' '$RUNTIME/app.log'"
check "AudioServer portal served"          "grep -q \"portal connect request '/tmp/session/0/portal/audio'\" '$RUNTIME/app.log'"
check "audio stream started at 44100 Hz"   "grep -q 'output stream started at 44100 Hz' '$RUNTIME/app.log'"
check "no fatal verification error"        "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "frame captured"                     "[[ -s '$FRAME_OUT' ]]"

if [[ -n "$PW_RUNTIME" ]]; then
    check "PipeWire reached streaming"     "grep -q 'stream state .* -> streaming' '$RUNTIME/app.log'"
    check "PipeWire process callback ran"  "grep -q 'process callback running' '$RUNTIME/app.log'"
    check "no client underruns"            "grep -q 'underruns=0' '$RUNTIME/app.log'"
else
    skip "PipeWire reached streaming" "no PipeWire server found"
fi

if [[ $failures -ne 0 ]]; then
    echo "==> $failures Piano check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> Piano test passed (frame: $FRAME_OUT)"
