#!/usr/bin/env bash
# Build just the headless-compositor target from the xlib-wayland checkout.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XLIB_WAYLAND_SRC="${XLIB_WAYLAND_SRC:-$PROJECT_ROOT/third_party/xlib-wayland}"
XLIB_WAYLAND_BUILD="${XLIB_WAYLAND_BUILD:-$PROJECT_ROOT/build/xlib-wayland}"

if [[ ! -d "$XLIB_WAYLAND_SRC/.git" ]]; then
    echo "error: xlib-wayland not found at $XLIB_WAYLAND_SRC" >&2
    echo "       run scripts/fetch-headless-compositor.sh first" >&2
    exit 1
fi

if [[ -f "$XLIB_WAYLAND_BUILD/build.ninja" ]]; then
    meson setup --reconfigure "$XLIB_WAYLAND_BUILD" "$XLIB_WAYLAND_SRC" --buildtype=release
else
    meson setup "$XLIB_WAYLAND_BUILD" "$XLIB_WAYLAND_SRC" --buildtype=release
fi

meson compile -C "$XLIB_WAYLAND_BUILD" headless-compositor

echo "==> Built $XLIB_WAYLAND_BUILD/headless-compositor"
