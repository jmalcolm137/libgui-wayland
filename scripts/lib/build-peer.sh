# Compile tools/wl-clipboard-peer.c with its protocol glue.
#
#   build_wl_clipboard_peer OUTPUT
#
# Both the headless and the clipboard suites use the peer; generating the
# protocols here keeps them from carrying the same wayland-scanner calls. The
# peer maps a 1x1 toplevel in its --primary modes (wlroots only sends PRIMARY to
# the focused client), so it needs xdg-shell as well.
build_wl_clipboard_peer() {
    local out="$1"
    local dir
    dir="$(dirname "$out")/peer-protocol"
    local protos
    protos="$(pkg-config --variable=pkgdatadir wayland-protocols)"
    local primary_xml="$protos/unstable/primary-selection/primary-selection-unstable-v1.xml"
    local xdg_xml="$protos/stable/xdg-shell/xdg-shell.xml"
    mkdir -p "$dir"
    wayland-scanner client-header "$primary_xml" "$dir/primary-selection-unstable-v1-client-protocol.h"
    wayland-scanner private-code "$primary_xml" "$dir/primary-selection-unstable-v1-protocol.c"
    wayland-scanner client-header "$xdg_xml" "$dir/xdg-shell-client-protocol.h"
    wayland-scanner private-code "$xdg_xml" "$dir/xdg-shell-protocol.c"
    cc -O2 -Wall -I "$dir" -o "$out" \
        "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/wl-clipboard-peer.c" \
        "$dir/primary-selection-unstable-v1-protocol.c" \
        "$dir/xdg-shell-protocol.c" \
        $(pkg-config --cflags --libs wayland-client)
}
