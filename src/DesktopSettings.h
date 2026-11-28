/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <pthread.h>

struct sd_bus;

namespace Core {
class Notifier;
}

namespace LibWM {

// The desktop's appearance preference, read from the standard
// org.freedesktop.portal.Settings portal on the session bus.
//
// This is the one part of "the desktop's settings" that has a portable API:
// `org.freedesktop.appearance color-scheme` (0 no preference, 1 prefer dark,
// 2 prefer light). Fonts, pointer/keyboard settings and the display layout have
// no such standard, so they remain the desktop's own (see the README).
//
// Only used when no desktop sends serenity-window host settings; those win.
// The object is created and driven on LibWM's server thread (LibWM's
// WindowServer connection lives there), on the event loop it was created with.
class DesktopSettings {
public:
    DesktopSettings();
    ~DesktopSettings();

    // Whether a Settings portal answered on this session bus.
    bool available() const;
    // org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer dark,
    // 2 prefer light.
    u32 color_scheme() const;
    // Called on the main thread when the colour scheme changes.
    void set_changed_callback(Function<void()>);

private:
    void read_color_scheme();
    void on_readable();

    sd_bus* m_bus { nullptr };
    RefPtr<Core::Notifier> m_notifier;
    bool m_available { false };
    u32 m_scheme { 0 };
    Function<void()> m_changed;
    mutable pthread_mutex_t m_mutex {};

    DesktopSettings(DesktopSettings const&) = delete;
    DesktopSettings& operator=(DesktopSettings const&) = delete;
};

}
