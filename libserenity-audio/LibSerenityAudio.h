/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace LibSerenityAudio {

// Install the in-process AudioServer portal, backed by PipeWire. Called
// automatically from a static initializer, so simply linking the library is
// enough. Safe to call more than once.
void initialize();

}
