/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace LibWM {

// Install the in-process portals (WindowServer, Clipboard, ...).
//
// This is idempotent and is also run automatically by a static initializer when
// the library is loaded, so that unmodified applications never have to call it.
void initialize();

}
