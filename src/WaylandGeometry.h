/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>

namespace LibWM {

// The surface-local coordinate for an output position on a surface whose
// output origin is `origin` and whose logical size is `size`, or an empty
// optional when the position is not inside it. Right and bottom edges are
// exclusive, as they are for surface sizes.
//
// wp_pointer_warp_v1 takes a position on one of the client's own surfaces, so
// mapping an output position to a surface needs this test first. It is a free
// function (rather than a lambda) so the boundary rules have a unit test.
inline Optional<Gfx::IntPoint> surface_local_point(Gfx::IntPoint position, Gfx::IntPoint origin, Gfx::IntSize size)
{
    auto local = position - origin;
    if (size.is_empty())
        return {};
    if (local.x() < 0 || local.y() < 0 || local.x() >= size.width() || local.y() >= size.height())
        return {};
    return local;
}

}
