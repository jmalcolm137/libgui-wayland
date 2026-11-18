#!/usr/bin/env bash
# Wayland window-state, HiDPI scaling, unit tests, and Linux portal/launcher
# integration tests.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERENITY_SRC="${SERENITY_SRC:-$PROJECT_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/lagom}"
HC="${HC:-$PROJECT_ROOT/build/xlib-wayland/headless-compositor}"

if [[ ! -x "$HC" ]]; then
    "$PROJECT_ROOT/scripts/build-headless-compositor.sh"
fi

ninja -C "$BUILD_DIR" \
    libwm-unit-test \
    libwm-test-window \
    libwm-fullscreen-test \
    libwm-hidpi-detail-test \
    libwm-appfile-test \
    Calculator >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/libwm-winportal.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/session/portal"
export XDG_RUNTIME_DIR="$RUNTIME"

cleanup() {
    kill "$HC_PID" 2>/dev/null || true
    [[ "${KEEP:-0}" == "1" ]] || rm -rf "$RUNTIME"
}
HC_PID=""
trap cleanup EXIT

failures=0
check() {
    if eval "$2"; then
        printf '%-50s PASS\n' "$1"
    else
        printf '%-50s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

# --- 1. Direct LibWM unit tests (MenuController, PNG transcoding, Config) ----
UNIT_EXIT=0
env XDG_RUNTIME_DIR="$RUNTIME" XDG_CONFIG_HOME="$RUNTIME/config" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY="" \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$BUILD_DIR/bin/libwm-unit-test" > "$RUNTIME/unit.log" 2>&1 || UNIT_EXIT=$?
check "unit: MenuController, PNG, Config & concurrency" "[[ $UNIT_EXIT -eq 0 ]] && grep -qE '[1-9][0-9]* passed, 0 failed' '$RUNTIME/unit.log'"
[[ $UNIT_EXIT -eq 0 ]] || grep -E 'FAIL|passed' "$RUNTIME/unit.log" || true

# --- 2. Window state transitions (fullscreen, maximize, minimize, resize) ----
cat > "$RUNTIME/fs.input" <<'EOF'
sleep 900
resize 800 600
sleep 800
EOF
"$HC" --socket libwm-fs --size 800x600 --timeout 4 \
    --output "$RUNTIME/fs.png" --input "$RUNTIME/fs.input" > "$RUNTIME/fs.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-fs" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-fs \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 5 "$BUILD_DIR/bin/libwm-fullscreen-test" > "$RUNTIME/fs.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

check "window-state: set_fullscreen(true) sent"        "grep -q 'LibWM: set_fullscreen(.*, true)' '$RUNTIME/fs.app.log'"
check "window-state: set_fullscreen(false) sent"       "grep -q 'LibWM: set_fullscreen(.*, false)' '$RUNTIME/fs.app.log'"
check "window-state: set_maximized(true) sent"         "grep -q 'LibWM: set_maximized(.*, true)' '$RUNTIME/fs.app.log'"
check "window-state: set_maximized(false) sent"        "grep -q 'LibWM: set_maximized(.*, false)' '$RUNTIME/fs.app.log'"
check "window-state: set_minimized(true) sent"         "grep -q 'LibWM: set_minimized(.*, true)' '$RUNTIME/fs.app.log'"
check "window-state: configure resized to 800x600"     "grep -q 'FULLSCREEN-TEST: resized to 800x600' '$RUNTIME/fs.app.log'"

# --- 2b. Client-Side Decorations (CSD) fallback ------------------------------
cat > "$RUNTIME/csd.input" <<'EOF'
sleep 900
# Click at surface (160, 142) -> content (160, 120) after subtracting 22px CSD titlebar
motion 160 142
button PRESS left
button RELEASE left
sleep 80
# Click on CSD titlebar drag area (100, 10) -> xdg_toplevel.move
motion 100 10
button PRESS left
button RELEASE left
sleep 80
# Click on CSD Maximize button (290, 10) -> set_maximized
motion 290 10
button PRESS left
button RELEASE left
sleep 80
# Click on CSD Close button (308, 10) -> window_close_request
motion 308 10
button PRESS left
button RELEASE left
sleep 300
EOF
"$HC" --socket libwm-csd --size 800x600 --timeout 3 \
    --output "$RUNTIME/csd.png" --input "$RUNTIME/csd.input" > "$RUNTIME/csd.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-csd" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-csd \
    LIBWM_FORCE_CSD=1 \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 4 "$BUILD_DIR/bin/libwm-test-window" > "$RUNTIME/csd.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

check "csd: client-side decorations enabled"           "grep -q 'using client-side decorations for window' '$RUNTIME/csd.app.log'"
check "csd: 22px titlebar composed above content"      "grep -q 'incl. 22px csd' '$RUNTIME/csd.app.log'"
check "csd: content click offset by 22px titlebar"     "grep -q 'TestWindow: mouse down at 160,120 button 1' '$RUNTIME/csd.app.log'"
check "csd: titlebar drag initiated xdg_toplevel.move" "grep -q 'CSD titlebar drag move on window' '$RUNTIME/csd.app.log'"
check "csd: maximize button toggled maximize"          "grep -q 'CSD maximize button clicked on window' '$RUNTIME/csd.app.log'"
check "csd: close button closed the window"            "grep -q 'CSD close button clicked on window' '$RUNTIME/csd.app.log'"

# --- 3. HiDPI output scale (1x vs 2x wl_output.scale & backing store) --------
"$HC" --socket libwm-hidpi-1x --size 800x600 --scale 1 --timeout 2 \
    --output "$RUNTIME/hc-1x.png" > "$RUNTIME/hidpi-1x.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-hidpi-1x" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-hidpi-1x \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    LIBWM_DUMP="$RUNTIME/dump-1x.png" \
    timeout 4 "$BUILD_DIR/bin/libwm-hidpi-detail-test" > "$RUNTIME/hidpi-1x.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

"$HC" --socket libwm-hidpi-2x --size 800x600 --scale 2 --timeout 2 \
    --output "$RUNTIME/hc-2x.png" > "$RUNTIME/hidpi-2x.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-hidpi-2x" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-hidpi-2x \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    LIBWM_DUMP="$RUNTIME/dump-2x.png" \
    timeout 4 "$BUILD_DIR/bin/libwm-hidpi-detail-test" > "$RUNTIME/hidpi-2x.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

check "hidpi: 1x output scale negotiated"              "grep -q 'output scale 1' '$RUNTIME/hidpi-1x.app.log'"
check "hidpi: 1x backing store is 200x96"              "grep -q 'dumped window .* (200x96)' '$RUNTIME/hidpi-1x.app.log'"
check "hidpi: 2x output scale negotiated"              "grep -q 'output scale 2' '$RUNTIME/hidpi-2x.app.log'"
check "hidpi: 2x backing store is 400x192"             "grep -q 'dumped window .* (400x192)' '$RUNTIME/hidpi-2x.app.log'"
check "hidpi: compositor captured 400x192 @2x buffer"  "grep -q 'captured 400x192' '$RUNTIME/hidpi-2x.hc.log'"

# Fractional HiDPI (1.5x): backing store renders at 2x (400x192) and LibWM
# resamples + presents a 300x144 (1.5x) buffer with wp_viewport destination 200x96.
"$HC" --socket libwm-hidpi-15x --size 800x600 --scale 1 --timeout 2 \
    --output "$RUNTIME/hc-15x.png" > "$RUNTIME/hidpi-15x.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-hidpi-15x" ]] && break; sleep 0.1; done

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-hidpi-15x \
    SDE_CLIENT_SCALE=1.5 \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    LIBWM_DUMP="$RUNTIME/dump-15x.png" \
    timeout 4 "$BUILD_DIR/bin/libwm-hidpi-detail-test" > "$RUNTIME/hidpi-15x.app.log" 2>&1 || true
wait "$HC_PID" 2>/dev/null || true
HC_PID=""

check "hidpi: 1.5x backing store rendered at 2x (400x192)" "grep -q 'dumped window .* (400x192)' '$RUNTIME/hidpi-15x.app.log'"
check "hidpi: 1.5x presented 300x144 fractional buffer"    "grep -q '@1.50x -> 300x144px' '$RUNTIME/hidpi-15x.app.log'"
check "hidpi: compositor captured 300x144 @1.5x buffer"    "grep -q 'captured 300x144' '$RUNTIME/hidpi-15x.hc.log'"

python3 - "$RUNTIME/dump-1x.png" "$RUNTIME/dump-2x.png" "$RUNTIME/hc-15x.png" <<'PY' > "$RUNTIME/hidpi-pixels.log" 2>&1
import struct, sys, zlib

def read_png_rgba(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    w = h = color_type = 0
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos+4])[0]
        tag = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+length]
        pos += 12 + length
        if tag == b"IHDR":
            w, h, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk)
            assert bit_depth == 8
        elif tag == b"IDAT":
            idat.extend(chunk)
    raw = zlib.decompress(bytes(idat))
    bpp = 4 if color_type == 6 else 3
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        ftype = raw[i]
        row = bytearray(raw[i+1:i+1+stride])
        i += 1 + stride
        if ftype == 1:
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 0xff
        elif ftype == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xff
        elif ftype == 3:
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                row[x] = (row[x] + ((a + b) >> 1)) & 0xff
        elif ftype == 4:
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[x] = (row[x] + pr) & 0xff
        prev = row
        rows.append((row, bpp))
    return w, h, rows

def pixel_rgb(rows, x, y):
    row, bpp = rows[y]
    off = x * bpp
    return (row[off], row[off+1], row[off+2])

w1, h1, rows1 = read_png_rgba(sys.argv[1])
w2, h2, rows2 = read_png_rgba(sys.argv[2])
w15, h15, rows15 = read_png_rgba(sys.argv[3])
assert (w1, h1) == (200, 96), f"1x size mismatch: {w1}x{h1}"
assert (w2, h2) == (400, 192), f"2x size mismatch: {w2}x{h2}"
assert (w15, h15) == (300, 144), f"1.5x size mismatch: {w15}x{h15}"
# Verify full-width drawing operations at 2x reach x=390 (near right edge of 400px buffer):
assert pixel_rgb(rows2, 390, 14) == (255, 0, 0), f"red rect at x=390: {pixel_rgb(rows2, 390, 14)}"
assert pixel_rgb(rows2, 390, 52) == (0, 128, 0), f"thin green line at x=390: {pixel_rgb(rows2, 390, 52)}"
assert pixel_rgb(rows2, 390, 72) == (255, 0, 255), f"thick magenta line at x=390: {pixel_rgb(rows2, 390, 72)}"
assert pixel_rgb(rows2, 390, 92) == (0, 0, 0), f"black rect outline at x=390: {pixel_rgb(rows2, 390, 92)}"
# Verify 1.5x resampled buffer (300x144) preserves crisp full-width lines to x=290:
assert pixel_rgb(rows15, 290, 10) == (255, 0, 0), f"1.5x red rect at x=290: {pixel_rgb(rows15, 290, 10)}"
assert pixel_rgb(rows15, 290, 39) == (0, 128, 0), f"1.5x thin green line at x=290: {pixel_rgb(rows15, 290, 39)}"
assert pixel_rgb(rows15, 290, 54) == (255, 0, 255), f"1.5x thick magenta line at x=290: {pixel_rgb(rows15, 290, 54)}"
assert pixel_rgb(rows15, 290, 69) == (0, 0, 0), f"1.5x black rect outline at x=290: {pixel_rgb(rows15, 290, 69)}"
print("HIDPI-PIXELS-OK")
PY
check "hidpi: 2x & 1.5x lines span full width crisply" "grep -q 'HIDPI-PIXELS-OK' '$RUNTIME/hidpi-pixels.log'"

# --- 4. Linux portal socket resilience & AppFile / LaunchServer spawning -----
# Create a stale (bound, never listened, closed) Unix domain socket at the
# session's clipboard & launch portal paths, plus a live listening socket at
# the notify portal path.
python3 - "$RUNTIME/session/portal/clipboard" "$RUNTIME/session/portal/launch" "$RUNTIME/session/portal/notify" <<'PY' &
import os, socket, sys, time
for stale_path in sys.argv[1:3]:
    if os.path.exists(stale_path):
        os.unlink(stale_path)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(stale_path)
    s.close()

notify_path = sys.argv[3]
if os.path.exists(notify_path):
    os.unlink(notify_path)
live = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
live.bind(notify_path)
live.listen(4)
time.sleep(5)
live.close()
PY
SOCK_HELPER_PID=$!
for _ in $(seq 1 50); do [[ -S "$RUNTIME/session/portal/notify" ]] && break; sleep 0.05; done

"$HC" --socket libwm-portal --size 800x600 --timeout 4 \
    --output "$RUNTIME/portal.png" > "$RUNTIME/portal.hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/libwm-portal" ]] && break; sleep 0.1; done

# Run libwm-appfile-test Calculator --launcher: exercises stale socket recovery on
# the launch portal AND LaunchServerConnection::open_url spawning /bin/Calculator.
env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-portal \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LIBWM_APP_BIN_DIR="$BUILD_DIR/bin" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 5 "$BUILD_DIR/bin/libwm-appfile-test" Calculator --launcher > "$RUNTIME/launcher.log" 2>&1 || true

# Also run direct AppFile::spawn_with_escalation path.
env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY=libwm-portal \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LIBWM_APP_BIN_DIR="$BUILD_DIR/bin" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 5 "$BUILD_DIR/bin/libwm-appfile-test" Calculator > "$RUNTIME/appfile.log" 2>&1 || true

# Exercise live external session socket deferral (notify) and SOCKET_TAKEOVER
# single-client (RequestServer) & multi-client (SQLServer) spawning.
mkdir -p "$RUNTIME/fake-bin"
cat > "$RUNTIME/fake-bin/RequestServer" <<EOF
#!/usr/bin/env python3
import os, stat
takeover = os.environ.get("SOCKET_TAKEOVER", "")
name, fd_str = takeover.split(":")
st = os.fstat(int(fd_str))
open("$RUNTIME/request-takeover.out", "w").write(f"{name}:{stat.S_ISSOCK(st.st_mode)}\n")
EOF
cat > "$RUNTIME/fake-bin/SQLServer" <<EOF
#!/usr/bin/env python3
import os, stat
takeover = os.environ.get("SOCKET_TAKEOVER", "")
name, fd_str = takeover.split(":")
st = os.fstat(int(fd_str))
open("$RUNTIME/sql-takeover.out", "w").write(f"{name}:{stat.S_ISSOCK(st.st_mode)}\n")
EOF
chmod +x "$RUNTIME/fake-bin/RequestServer" "$RUNTIME/fake-bin/SQLServer"

env XDG_RUNTIME_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" WAYLAND_DISPLAY="" \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LIBWM_APP_BIN_DIR="$RUNTIME/fake-bin" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 5 "$BUILD_DIR/bin/libwm-appfile-test" --test-portals > "$RUNTIME/portals.log" 2>&1 || true
for _ in $(seq 1 30); do [[ -s "$RUNTIME/request-takeover.out" && -s "$RUNTIME/sql-takeover.out" ]] && break; sleep 0.05; done

wait "$HC_PID" 2>/dev/null || true
wait "$SOCK_HELPER_PID" 2>/dev/null || true
HC_PID=""
pkill -f "$BUILD_DIR/bin/Calculator" 2>/dev/null || true

check "portal: stale Unix socket detected & unlinked"  "grep -q \"portal socket '.*/session/portal/launch' is stale; serving in-process\" '$RUNTIME/launcher.log'"
check "portal: live session socket deferred to"        "grep -q \"deferring to the session service on '.*/session/portal/notify'\" '$RUNTIME/portals.log'"
check "portal: single-client SOCKET_TAKEOVER spawned"  "grep -q 'requestserver:True' '$RUNTIME/request-takeover.out'"
check "portal: multi-client SOCKET_TAKEOVER spawned"   "grep -q 'sql:True' '$RUNTIME/sql-takeover.out'"
check "portal: LaunchServer launched /bin/Calculator"  "grep -q 'LaunchServer: launched executable .*/bin/Calculator' '$RUNTIME/launcher.log'"
check "portal: Desktop::Launcher::open succeeded"      "grep -q \"LAUNCHER-OPEN: opened 'Calculator' (/bin/Calculator)\" '$RUNTIME/launcher.log'"
check "portal: AppFile::spawn_with_escalation worked"  "grep -q \"APPFILE-LAUNCH: spawned 'Calculator' (/bin/Calculator)\" '$RUNTIME/appfile.log'"

if [[ $failures -ne 0 ]]; then
    KEEP=1
    echo "==> $failures check(s) failed; logs in $RUNTIME"
    exit 1
fi
echo "==> window-state, HiDPI, unit & portal tests passed"
