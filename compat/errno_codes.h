#pragma once

// libgui-wayland host compat: SerenityOS has a generated `LibC/errno_codes.h`;
// on the host the standard <errno.h> provides everything applications need.
#include <errno.h>
