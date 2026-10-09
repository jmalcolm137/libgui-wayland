#!/usr/bin/env bash
# Fetch the headless Wayland compositor from the xlib-wayland project.
#
# It is a pure-Wayland tool (libwayland-server only; no X11), so although it
# lives in xlib-wayland we can use it unchanged as our test compositor. We
# depend on it as an external project rather than vendoring it, so that any
# changes we need can be pushed back to xlib-wayland.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XLIB_WAYLAND_REMOTE="${XLIB_WAYLAND_REMOTE:-https://github.com/jmalcolm137/xlib-wayland.git}"
XLIB_WAYLAND_REVISION="${XLIB_WAYLAND_REVISION:-ddb3f1def720ea00bb0e05d9bc7e12c354a25bca}"
XLIB_WAYLAND_SRC="${XLIB_WAYLAND_SRC:-$PROJECT_ROOT/third_party/xlib-wayland}"

if [[ -d "$XLIB_WAYLAND_SRC/.git" ]]; then
    echo "==> Updating xlib-wayland at $XLIB_WAYLAND_SRC"
    git -C "$XLIB_WAYLAND_SRC" fetch origin "$XLIB_WAYLAND_REVISION"
else
    echo "==> Cloning xlib-wayland into $XLIB_WAYLAND_SRC"
    git clone --filter=blob:none "$XLIB_WAYLAND_REMOTE" "$XLIB_WAYLAND_SRC"
    git -C "$XLIB_WAYLAND_SRC" fetch origin "$XLIB_WAYLAND_REVISION"
fi

git -C "$XLIB_WAYLAND_SRC" checkout --quiet "$XLIB_WAYLAND_REVISION"
echo "==> xlib-wayland ready at $XLIB_WAYLAND_SRC ($(git -C "$XLIB_WAYLAND_SRC" rev-parse --short HEAD))"
