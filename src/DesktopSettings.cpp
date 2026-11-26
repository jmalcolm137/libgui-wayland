/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DesktopSettings.h"
#include "Environment.h"
#include <string.h>

#ifdef LIBWM_HAVE_SDBUS
#    include <LibCore/Notifier.h>
#    include <systemd/sd-bus.h>
#endif

namespace LibWM {

#ifdef LIBWM_HAVE_SDBUS

namespace {

constexpr char const* portal_service = "org.freedesktop.portal.Desktop";
constexpr char const* portal_path = "/org/freedesktop/portal/desktop";
constexpr char const* portal_interface = "org.freedesktop.portal.Settings";
constexpr char const* appearance_namespace = "org.freedesktop.appearance";
constexpr char const* color_scheme_key = "color-scheme";

}

DesktopSettings::DesktopSettings()
{
    pthread_mutex_init(&m_mutex, nullptr);

    if (sd_bus_open_user(&m_bus) < 0) {
        // No session bus (a bare session, a container): nothing to read.
        trace("LibWM: no session bus; not following the desktop appearance");
        m_bus = nullptr;
        return;
    }

    trace("LibWM: session bus open for desktop appearance");
    sd_bus_set_method_call_timeout(m_bus, 250 * 1000);
    // Colour-scheme changes arrive as SettingChanged; the other portal keys are
    // not relevant here.
    (void)sd_bus_add_match(m_bus, nullptr,
        "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged'",
        nullptr, nullptr);

    // The first read is synchronous and bounded: a portal answers in
    // microseconds, and with no portal the call fails at once because the bus
    // reports the missing service. This runs on LibWM's server thread before its
    // event loop starts, where there is nothing else to dispatch yet.
    read_color_scheme();

    // Later changes are signalled; process the bus from the event loop this
    // object was created on (LibWM's server thread).
    if (auto fd = sd_bus_get_fd(m_bus); fd >= 0) {
        m_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        m_notifier->on_activation = [this] { on_readable(); };
    }
}

DesktopSettings::~DesktopSettings()
{
    m_notifier = nullptr;
    if (m_bus)
        sd_bus_unref(m_bus);
    pthread_mutex_destroy(&m_mutex);
}

void DesktopSettings::read_color_scheme()
{
    if (!m_bus)
        return;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int const result = sd_bus_call_method(m_bus, portal_service, portal_path, portal_interface, "Read",
        &error, &reply, "ss", appearance_namespace, color_scheme_key);
    if (result < 0) {
        // No portal on this bus, or no such key: keep the bundled default theme,
        // as before. (`sd_bus_open_user` succeeded, so this is not an error.)
        if (error.message)
            trace("LibWM: Settings portal read failed ({})", error.message);
        else
            trace("LibWM: no Settings portal on the session bus");
        sd_bus_error_free(&error);
        return;
    }

    uint32_t scheme = 0;
    bool parsed = false;
    // Read returns a variant; enter it to read the uint32 inside.
    if (sd_bus_message_enter_container(reply, 'v', nullptr) > 0) {
        parsed = sd_bus_message_read(reply, "u", &scheme) > 0;
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (!parsed)
        return;

    bool const was_available = m_available;
    pthread_mutex_lock(&m_mutex);
    bool const changed = scheme != m_scheme;
    m_scheme = scheme;
    pthread_mutex_unlock(&m_mutex);
    m_available = true;

    trace("LibWM: Settings portal colour-scheme={} ({})", scheme,
        scheme == 1 ? "prefer dark"sv : scheme == 2 ? "prefer light"sv : "no preference"sv);
    if ((changed || !was_available) && m_changed)
        m_changed();
}

void DesktopSettings::on_readable()
{
    if (!m_bus)
        return;
    bool color_scheme_changed = false;
    sd_bus_message* message = nullptr;
    while (sd_bus_process(m_bus, &message) > 0) {
        if (message) {
            if (sd_bus_message_is_signal(message, portal_interface, "SettingChanged")) {
                char const* name_space = nullptr;
                char const* key = nullptr;
                if (sd_bus_message_read(message, "ss", &name_space, &key) > 0
                    && name_space && key
                    && !strcmp(name_space, appearance_namespace)
                    && !strcmp(key, color_scheme_key))
                    color_scheme_changed = true;
            }
            sd_bus_message_unref(message);
            message = nullptr;
        }
    }
    if (color_scheme_changed)
        read_color_scheme();
}

bool DesktopSettings::available() const
{
    return m_available;
}

u32 DesktopSettings::color_scheme() const
{
    pthread_mutex_lock(&m_mutex);
    u32 const value = m_scheme;
    pthread_mutex_unlock(&m_mutex);
    return value;
}

void DesktopSettings::set_changed_callback(Function<void()> callback)
{
    m_changed = move(callback);
}

#else

DesktopSettings::DesktopSettings()
{
    pthread_mutex_init(&m_mutex, nullptr);
}
DesktopSettings::~DesktopSettings()
{
    pthread_mutex_destroy(&m_mutex);
}
bool DesktopSettings::available() const { return false; }
u32 DesktopSettings::color_scheme() const { return 0; }
void DesktopSettings::set_changed_callback(Function<void()>) { }

#endif

}
