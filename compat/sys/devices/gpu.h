#pragma once

// libgui-wayland host compat: the graphics-device ioctls SerenityOS exposes via
// <sys/devices/gpu.h>. Nothing on the host backs these, so the helpers fail
// cleanly (the callers already handle that).
#include <stddef.h>

struct GraphicsHeadEDID {
    unsigned char* bytes;
    unsigned bytes_size;
};

static inline int graphics_connector_get_head_edid(int, GraphicsHeadEDID*)
{
    return -1;
}
