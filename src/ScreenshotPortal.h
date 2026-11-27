/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>

struct sd_bus;

namespace LibWM {

// Screen capture through org.freedesktop.portal.Screenshot: the portable route
// when the compositor does not provide serenity-window's capture. Only used
// from LibWM's server thread; the last frame is cached briefly for the
// Magnifier.
class ScreenshotPortal {
public:
    static ScreenshotPortal& the();

    // The whole screen as a bitmap, or nullptr when no portal answers.
    RefPtr<Gfx::Bitmap> capture(int timeout_ms = 2000);

private:
    ScreenshotPortal();
    ~ScreenshotPortal();
    ScreenshotPortal(ScreenshotPortal const&) = delete;
    ScreenshotPortal& operator=(ScreenshotPortal const&) = delete;

    sd_bus* m_bus { nullptr };
    RefPtr<Gfx::Bitmap> m_last_capture;
    u64 m_last_capture_ms { 0 };
    unsigned m_request_count { 0 };
    bool m_reported_failure { false };
};

} // namespace LibWM
