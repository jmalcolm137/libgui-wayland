#!/usr/bin/env bash
# Headless Wayland input & drag-and-drop integration test:
#   - wl_shm presentation & frame capture
#   - wl_pointer enter, motion, button press/release, double-click synthesis
#   - wl_pointer axis (vertical & horizontal scroll wheel accumulation)
#   - wl_keyboard XKB translation (Shift+A -> 'A', Ctrl+C -> ASCII ETX 3)
#   - wl_data_device outgoing drag-and-drop (GUI::DragOperation -> Wayland)
#   - wl_data_device incoming external drag-and-drop (Wayland peer -> drop_event)
set -euo pipefail

# The checks below assert on LibWM's trace output (see Environment.h).
export LIBWM_TRACE="${LIBWM_TRACE:-1}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"
PEER_BIN="${PEER_BIN:-$PROJECT_ROOT/build/wl-clipboard-peer}"
APP="${APP:-$BUILD_DIR/bin/libwm-test-window}"
FRAME_OUT="${FRAME_OUT:-$PROJECT_ROOT/build/headless-frame.png}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi
cc -O2 -Wall -o "$PEER_BIN" "$PROJECT_ROOT/tools/wl-clipboard-peer.c" $(pkg-config --cflags --libs wayland-client)
ninja -C "$BUILD_DIR" libwm-test-window libwm-resize-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-headless.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/session/portal"
trap '[[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"' EXIT

cat > "$RUNTIME/input.txt" <<'EOF'
sleep 1200
# Two quick clicks at (160,120) -> click + synthesized double-click
motion 160 120
button PRESS left
button RELEASE left
sleep 80
button PRESS left
button RELEASE left
sleep 100
# Scroll wheel: vertical (+2) and horizontal (-1)
axis vertical 2
sleep 80
axis horizontal -1
sleep 100
# Shift (42) + A (30) -> 'A' (code_point=65)
key PRESS 42
key PRESS 30
key RELEASE 30
key RELEASE 42
sleep 80
# Ctrl (29) + C (46) -> ETX (code_point=3)
key PRESS 29
key PRESS 46
key RELEASE 46
key RELEASE 29
sleep 100
# Outgoing drag: right-press starts GUI::DragOperation, motion moves it, release drops it
button PRESS right
sleep 120
motion 180 140
sleep 120
button RELEASE right
sleep 200
EOF

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket libwm-test --size 800x600 --timeout 5 \
    --output "$FRAME_OUT" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!

for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-test" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-test \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 7 "$APP" > "$RUNTIME/app.log" 2>&1 || true

wait "$HC_PID" 2>/dev/null || true

# Scenario 2: Incoming external Wayland drag-and-drop (wl-clipboard-peer -> LibWM window)
cat > "$RUNTIME/dnd-input.txt" <<'EOF'
sleep 1800
motion 140 110
sleep 150
motion 145 115
sleep 150
button RELEASE left
sleep 400
EOF

"$HC" --socket libwm-dnd --size 800x600 --timeout 5 \
    --output "$RUNTIME/dnd-frame.png" --input "$RUNTIME/dnd-input.txt" > "$RUNTIME/dnd-hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-dnd" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-dnd \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 6 "$APP" > "$RUNTIME/dnd-app.log" 2>&1 &
APP_PID=$!

for _ in $(seq 1 50); do grep -q "presented window" "$RUNTIME/dnd-app.log" 2>/dev/null && break; sleep 0.1; done

# Besides text/plain the peer offers a MIME type that is not valid UTF-8 (which
# used to abort the receiving app) and one it never writes (a stuck source,
# which must time out instead of freezing the app); the drop must still work.
WAYLAND_DISPLAY=libwm-dnd timeout 5 "$PEER_BIN" drag "peer-dropped-payload" \
    $'application/x-libwm-\xff\xfe-not-utf8' application/x-libwm-stall > "$RUNTIME/dnd-peer.log" 2>&1 &
PEER_PID=$!

wait "$APP_PID" 2>/dev/null || true
wait "$PEER_PID" 2>/dev/null || true
wait "$HC_PID" 2>/dev/null || true

# Scenario 3: interactive resize. A statusbar corner asks for a bottom-right
# resize (start_window_resize), which LibWM must turn into xdg_toplevel.resize
# with the serial of the press; the "None" direction must send nothing.
cat > "$RUNTIME/resize-input.txt" <<'EOF'
sleep 200
motion 200 150
button PRESS left
button RELEASE left
sleep 1500
EOF

"$HC" --socket libwm-resize --size 800x600 --timeout 3 \
    --input "$RUNTIME/resize-input.txt" > "$RUNTIME/resize-hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-resize" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-resize \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 4 "$BUILD_DIR/bin/libwm-resize-test" > "$RUNTIME/resize-app.log" 2>&1 || true

wait "$HC_PID" 2>/dev/null || true
HC_PID=""

# Scenario 4: no input yet. xdg_toplevel.resize needs the serial of the press
# that asked for it, so LibWM must decline instead of sending a bad request.
cat > "$RUNTIME/resize-noserial-input.txt" <<'EOF'
sleep 1500
EOF

"$HC" --socket libwm-resize-noserial --size 800x600 --timeout 3 \
    --input "$RUNTIME/resize-noserial-input.txt" > "$RUNTIME/resize-noserial-hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-resize-noserial" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" LIBWM_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-resize-noserial \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 4 "$BUILD_DIR/bin/libwm-resize-test" > "$RUNTIME/resize-noserial-app.log" 2>&1 || true

wait "$HC_PID" 2>/dev/null || true
HC_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-48s PASS\n' "$1"
    else
        printf '%-48s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "window presented over wl_shm"             "grep -q 'presented window' '$RUNTIME/app.log'"
check "minimum size forwarded to xdg_toplevel"   "grep -q 'set_min_size window .* -> 120x90' '$RUNTIME/app.log'"
check "window modified state round-tripped"      "grep -q 'TestWindow: modified=true min_size=120x90' '$RUNTIME/app.log'"
check "standard Hand cursor set with hotspot"    "grep -q 'set cursor (.*, hotspot 8,4)' '$RUNTIME/app.log'"
check "custom 12x12 bitmap cursor applied"       "grep -q 'set cursor (12x12, hotspot 6,6)' '$RUNTIME/app.log'"
check "alpha hit-threshold input region applied" "grep -q 'applied alpha input region on window .* (threshold=0.50' '$RUNTIME/app.log'"
check "opaque region sent once, not per frame"   "[ \"\$(grep -c 'set opaque region on window' '$RUNTIME/app.log')\" -eq 1 ] && [ \"\$(grep -c 'presented window' '$RUNTIME/app.log')\" -gt 1 ]"
check "pointer entered the window"               "grep -q 'pointer entered window' '$RUNTIME/app.log'"
check "synthetic click reached widget"           "grep -q 'TestWindow: mouse down at 160,120 button 1' '$RUNTIME/app.log'"
check "double-click synthesized and delivered"   "grep -q 'TestWindow: mouse double click at 160,120 button 1' '$RUNTIME/app.log'"
check "vertical wheel axis delivered"            "grep -q 'TestWindow: mouse wheel delta=0,20' '$RUNTIME/app.log'"
check "horizontal wheel axis delivered"          "grep -q 'TestWindow: mouse wheel delta=-10,0' '$RUNTIME/app.log'"
check "XKB Shift+A translated to 'A' (65)"       "grep -q 'TestWindow: key down code_point=65 .* modifiers=4' '$RUNTIME/app.log'"
check "XKB Ctrl+C translated to ETX (3)"         "grep -q 'TestWindow: key down code_point=3 .* modifiers=2' '$RUNTIME/app.log'"
check "outgoing drag started on Wayland"         "grep -q 'started drag (' '$RUNTIME/app.log'"
check "outgoing drag finished cleanly"           "grep -q 'TestWindow: outgoing drag finished' '$RUNTIME/app.log'"
check "oversized UTF-8 window title survived"    "grep -q 'TestWindow: oversized UTF-8 title clamped and sent' '$RUNTIME/app.log'"
check "shim keeps the full title (6600 bytes)"    "grep -q 'TestWindow: shim state title_bytes=6600 ' '$RUNTIME/app.log'"
check "shim owns min size; alpha from store"     "grep -q 'TestWindow: shim state .* min_size=120x90 alpha=true' '$RUNTIME/app.log'"
check "sync WindowServer requests all answer"   "grep -q 'TestWindow: sync requests answered minimized=false->true screen_bitmap=false theme_override=true overridden=true cleared=true' '$RUNTIME/app.log'"
check "zwp_text_input_v3 preedit received"       "grep -q \"TestWindow: IME preedit='ni'\" '$RUNTIME/app.log'"
check "zwp_text_input_v3 commit received"        "grep -q \"TestWindow: IME commit='你好'\" '$RUNTIME/app.log'"
check "connection-lost callback fired on close"  "grep -q 'TestWindow: connection lost reason=' '$RUNTIME/app.log'"
check "incoming Wayland drag entered widget"     "grep -q 'TestWindow: drag move at' '$RUNTIME/dnd-app.log'"
check "incoming Wayland drop transferred text"   "grep -q \"TestWindow: drop at 145,115 text='peer-dropped-payload'\" '$RUNTIME/dnd-app.log'"
check "external drag source got dnd_finished"    "grep -q 'PEER: dnd_finished' '$RUNTIME/dnd-peer.log'"
check "stuck drag source timed out, not hung"     "grep -q 'PEER: stalling' '$RUNTIME/dnd-peer.log' && grep -q \"drag data for 'application/x-libwm-stall' not received\" '$RUNTIME/dnd-app.log'"
check "resize: corner request became xdg_toplevel.resize" "grep -q 'started interactive resize of window .* (direction=6, edges=0xa, serial=[1-9]' '$RUNTIME/resize-app.log'"
check "resize: no request for direction None"     "[ \$(grep -c 'started interactive resize' '$RUNTIME/resize-app.log') -eq 1 ]"
check "resize: no seat serial declines the request" "grep -q 'no input serial; not resizing window' '$RUNTIME/resize-noserial-app.log'"
check "resize: nothing sent without a serial"     "! grep -q 'started interactive resize' '$RUNTIME/resize-noserial-app.log'"
check "frame captured to PNG"                    "[[ -s '$FRAME_OUT' ]]"

if grep -q 'captured' "$RUNTIME/hc.log"; then
    grep 'captured' "$RUNTIME/hc.log" | tail -1
fi

if [[ $failures -ne 0 ]]; then
    echo "==> $failures headless check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    echo "--- dnd app log ---"
    cat "$RUNTIME/dnd-app.log"
    echo "--- dnd peer log ---"
    cat "$RUNTIME/dnd-peer.log"
    exit 1
fi
echo "==> headless input & DnD test passed (frame: $FRAME_OUT)"
