/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "serenity-window-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-output-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace LibWM {

// --- registry -----------------------------------------------------------------

static void registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    auto& self = *static_cast<WaylandClient*>(data);

    if (!strcmp(interface, wl_compositor_interface.name))
        self.set_compositor(reinterpret_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, min(version, 4u))));
    else if (!strcmp(interface, wl_subcompositor_interface.name))
        self.set_subcompositor(reinterpret_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1)));
    else if (!strcmp(interface, wl_shm_interface.name))
        self.set_shm(reinterpret_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1)));
    else if (!strcmp(interface, wl_seat_interface.name))
        self.add_seat(reinterpret_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1)));
    else if (!strcmp(interface, wl_output_interface.name))
        self.add_output(reinterpret_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, min(version, 2u))));
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        self.add_wm_base(reinterpret_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1)));
    else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
        self.set_decoration_manager(reinterpret_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, min(version, 1u))));
    else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
        dbgln("LibWM/Wayland: found wl_data_device_manager v{}", version);
        self.set_data_device_manager(reinterpret_cast<wl_data_device_manager*>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, min(version, 3u))));
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        self.set_xdg_output_manager(reinterpret_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, min(version, 3u))));
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        self.set_viewporter(reinterpret_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1)));
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        self.set_layer_shell(reinterpret_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, min(version, 4u))));
    } else if (!strcmp(interface, serenity_window_manager_interface.name)) {
        self.set_serenity_window_manager(reinterpret_cast<serenity_window_manager*>(wl_registry_bind(registry, name, &serenity_window_manager_interface, min(version, 3u))));
    } else if (!strcmp(interface, zwp_text_input_manager_v3_interface.name)) {
        self.set_text_input_manager(reinterpret_cast<zwp_text_input_manager_v3*>(wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1)));
    }
}

static void registry_global_remove(void*, wl_registry*, uint32_t) { }

static wl_registry_listener const s_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- xdg_wm_base --------------------------------------------------------------

static void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial)
{
    dbgln("LibWM/Wayland: xdg_wm_base ping serial={} -> pong", serial);
    xdg_wm_base_pong(base, serial);
}

static xdg_wm_base_listener const s_wm_base_listener = {
    .ping = wm_base_ping,
};

// --- wl_output ----------------------------------------------------------------

static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, char const*, char const*, int32_t) { }

static void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t)
{
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    static_cast<WaylandClient*>(data)->on_output_mode({ width, height });
}

static void output_done(void* data, wl_output*)
{
    static_cast<WaylandClient*>(data)->on_output_done();
}

static void output_scale(void* data, wl_output*, int32_t factor)
{
    static_cast<WaylandClient*>(data)->on_output_scale(factor);
}

static void output_name(void*, wl_output*, char const*) { }
static void output_description(void*, wl_output*, char const*) { }

static wl_output_listener const s_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

// --- zxdg_output_v1 (logical size under fractional scaling) -------------------

static void xdg_output_logical_position(void*, zxdg_output_v1*, int32_t, int32_t) { }

static void xdg_output_logical_size(void* data, zxdg_output_v1*, int32_t width, int32_t height)
{
    static_cast<WaylandClient*>(data)->on_output_logical_size(nullptr, { width, height });
}

static void xdg_output_done(void*, zxdg_output_v1*) { }
static void xdg_output_name(void*, zxdg_output_v1*, char const*) { }
static void xdg_output_description(void*, zxdg_output_v1*, char const*) { }

static zxdg_output_v1_listener const s_xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_logical_size,
    .done = xdg_output_done,
    .name = xdg_output_name,
    .description = xdg_output_description,
};

// --- serenity_window_manager --------------------------------------------------

static void serenity_window_manager_host_config(void* data, serenity_window_manager*,
    uint32_t workspace_rows, uint32_t workspace_columns, wl_fixed_t accel_min, wl_fixed_t accel_max,
    uint32_t scroll_step_min, int32_t double_click_min, int32_t double_click_max)
{
    static_cast<WaylandClient*>(data)->on_host_config(workspace_rows, workspace_columns,
        wl_fixed_to_double(accel_min), wl_fixed_to_double(accel_max),
        scroll_step_min, double_click_min, double_click_max);
}

static void serenity_window_manager_wallpaper(void* data, serenity_window_manager*, int32_t width, int32_t height, int32_t pitch, uint32_t format, int32_t fd)
{
    static_cast<WaylandClient*>(data)->on_host_wallpaper(width, height, pitch, format, fd);
}

static void serenity_window_manager_host_settings(void* data, serenity_window_manager*, char const* settings)
{
    static_cast<WaylandClient*>(data)->on_host_settings(StringView { settings, strlen(settings) });
}

static void serenity_window_manager_dismiss_menus(void* data, serenity_window_manager*)
{
    static_cast<WaylandClient*>(data)->on_dismiss_menus();
}

static void serenity_window_manager_captured(void* data, serenity_window_manager*, uint32_t serial, int32_t width, int32_t height, int32_t pitch, uint32_t format, int32_t fd)
{
    static_cast<WaylandClient*>(data)->on_captured(serial, width, height, pitch, format, fd);
}

static serenity_window_manager_listener const s_serenity_window_manager_listener = {
    .host_settings = serenity_window_manager_host_settings,
    .wallpaper = serenity_window_manager_wallpaper,
    .dismiss_menus = serenity_window_manager_dismiss_menus,
    .host_config = serenity_window_manager_host_config,
    .captured = serenity_window_manager_captured,
};

void WaylandClient::set_serenity_window_manager(serenity_window_manager* manager)
{
    MutexLocker locker(m_mutex);
    m_serenity_window_manager = manager;
    if (manager)
        serenity_window_manager_add_listener(manager, &s_serenity_window_manager_listener, this);
}

void WaylandClient::on_host_config(u32 workspace_rows, u32 workspace_columns, double accel_min, double accel_max, u32 scroll_step_min, int double_click_min, int double_click_max)
{
    MutexLocker locker(m_mutex);
    m_host_config.workspace_rows = workspace_rows > 0 ? workspace_rows : 1;
    m_host_config.workspace_columns = workspace_columns > 0 ? workspace_columns : 1;
    m_host_config.accel_min = accel_min;
    m_host_config.accel_max = accel_max;
    m_host_config.scroll_step_min = scroll_step_min;
    m_host_config.double_click_min = double_click_min;
    m_host_config.double_click_max = double_click_max;
    dbgln("LibWM: host config: workspaces {}x{}, accel {}-{}, scroll min {}, double-click {}-{}",
        m_host_config.workspace_rows, m_host_config.workspace_columns, m_host_config.accel_min, m_host_config.accel_max,
        m_host_config.scroll_step_min, m_host_config.double_click_min, m_host_config.double_click_max);
}

void WaylandClient::on_host_wallpaper(i32 width, i32 height, i32 pitch, u32 format, i32 fd)
{
    MutexLocker locker(m_mutex);
    m_host_wallpaper = nullptr;
    if (fd < 0 || width <= 0 || height <= 0 || pitch <= 0)
        return;
    auto size = static_cast<size_t>(pitch) * static_cast<size_t>(height);
    auto* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped != MAP_FAILED) {
        if (auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(format), { width, height }); !bitmap.is_error()) {
            auto& created = *bitmap.value();
            auto const row_bytes = min(static_cast<size_t>(pitch), static_cast<size_t>(created.pitch()));
            for (int y = 0; y < height; ++y)
                memcpy(created.scanline(y), static_cast<u8 const*>(mapped) + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);
            m_host_wallpaper = bitmap.release_value();
        }
        munmap(mapped, size);
    }
    close(fd);
    dbgln("LibWM: host wallpaper {}x{}", width, height);
}

void WaylandClient::on_captured(u32, i32 width, i32 height, i32 pitch, u32 format, i32 fd)
{
    MutexLocker locker(m_mutex);
    if (fd >= 0 && width > 0 && height > 0 && pitch > 0) {
        auto const size = static_cast<size_t>(pitch) * static_cast<size_t>(height);
        auto* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
            if (auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(format), { width, height }); !bitmap.is_error()) {
                auto& created = *bitmap.value();
                auto const row_bytes = min(static_cast<size_t>(pitch), static_cast<size_t>(created.pitch()));
                for (int y = 0; y < height; ++y)
                    memcpy(created.scanline(y), static_cast<u8 const*>(mapped) + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);
                m_captured = bitmap.release_value();
            }
            munmap(mapped, size);
        }
    }
    if (fd >= 0)
        close(fd);
}

void WaylandClient::request_capture(Gfx::IntRect const& region)
{
    MutexLocker locker(m_mutex);
    if (!m_serenity_window_manager || region.is_empty())
        return;
    serenity_window_manager_request_capture(m_serenity_window_manager, ++m_capture_serial,
        region.x(), region.y(), region.width(), region.height());
    wl_display_flush(m_display);
}

void WaylandClient::set_host_wallpaper(Gfx::Bitmap* bitmap)
{
    MutexLocker locker(m_mutex);
    int fd = -1;
    if (bitmap) {
        auto name = ByteString::formatted("libwm-wallpaper-{}", getpid());
        fd = memfd_create(name.characters(), 0);
        if (fd >= 0) {
            auto size = static_cast<size_t>(bitmap->pitch()) * static_cast<size_t>(bitmap->height());
            auto* mapped = MAP_FAILED;
            if (ftruncate(fd, static_cast<off_t>(size)) == 0)
                mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapped != MAP_FAILED) {
                memcpy(mapped, bitmap->scanline(0), size);
                munmap(mapped, size);
            } else {
                close(fd);
                fd = -1;
            }
        }
    }
    if (m_serenity_window_manager) {
        // A `h` argument must be a real file descriptor: libwayland cannot dup a
        // -1, and aborts the whole request (breaking this connection with it).
        // "No wallpaper" is expressed by a zero width/height, so hand over a
        // valid placeholder fd in that case.
        if (fd < 0)
            fd = memfd_create("libwm-wallpaper-none", MFD_CLOEXEC);
        serenity_window_manager_set_wallpaper(m_serenity_window_manager,
            bitmap ? bitmap->width() : 0, bitmap ? bitmap->height() : 0,
            bitmap ? bitmap->pitch() : 0,
            bitmap ? static_cast<uint32_t>(bitmap->format()) : 0, fd);
        wl_display_flush(m_display);
    }
    if (fd >= 0)
        close(fd);
    // Keep our own answers current.
    m_host_wallpaper = bitmap;
}

void WaylandClient::on_host_settings(StringView blob)
{
    MutexLocker locker(m_mutex);
    m_host_settings.clear();
    for (auto line : blob.split_view('\n')) {
        if (line.is_empty())
            continue;
        auto separator = line.find('=');
        if (!separator.has_value())
            continue;
        m_host_settings.set(line.substring_view(0, *separator).to_byte_string(), line.substring_view(*separator + 1).to_byte_string());
    }
    dbgln("LibWM: host settings: {} entries", m_host_settings.size());
    if (on_host_settings_updated)
        on_host_settings_updated();
}

ByteString WaylandClient::host_setting(StringView key, StringView fallback) const
{
    MutexLocker locker(m_mutex);
    if (auto it = m_host_settings.find(ByteString { key }); it != m_host_settings.end())
        return ByteString { it->value.view() };
    return ByteString { fallback };
}

void WaylandClient::set_host_setting(StringView key, StringView value)
{
    MutexLocker locker(m_mutex);
    // Keep our own answers current, and hand the change to the desktop, which
    // applies and persists it.
    m_host_settings.set(ByteString { key }, ByteString { value });
    if (m_serenity_window_manager) {
        auto k = key.to_byte_string();
        auto v = value.to_byte_string();
        serenity_window_manager_set_host_setting(m_serenity_window_manager, k.characters(), v.characters());
        wl_display_flush(m_display);
    }
}

void WaylandClient::on_dismiss_menus()
{
    MutexLocker locker(m_mutex);
    if (m_dismiss_menus)
        m_dismiss_menus();
}

// --- connection & output lifecycle --------------------------------------------

WaylandClient::WaylandClient()
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}

WaylandClient::~WaylandClient()
{
    pthread_mutex_destroy(&m_mutex);
}

WaylandClient& WaylandClient::the()
{
    // Intentionally never destroyed, matching the portal server slots in
    // LibWM.cpp: the detached LibWM server thread calls into WaylandClient
    // and tearing it down during __cxa_finalize races that thread at exit.
    static auto* s_client = new WaylandClient;
    return *s_client;
}

ErrorOr<void> WaylandClient::ensure_connected()
{
    MutexLocker locker(m_mutex);
    if (m_display)
        return {};

    m_display = wl_display_connect(nullptr);
    if (!m_display)
        return Error::from_string_literal("LibWM: wl_display_connect failed (no compositor?)");

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registry_listener, this);

    // First roundtrip: enumerate globals. Second: collect wl_output state.
    if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0)
        return Error::from_string_literal("LibWM: wl_display_roundtrip failed");

    if (!m_compositor || !m_wm_base || !m_shm)
        return Error::from_string_literal("LibWM: compositor lacks required globals");

    m_notifier = Core::Notifier::construct(wl_display_get_fd(m_display), Core::Notifier::Type::Read);
    m_notifier->on_activation = [this] { dispatch(); };

    dbgln("LibWM/Wayland: connected; screen {}x{} @{}x, output scale {}",
        m_output_state.screen_size.width(), m_output_state.screen_size.height(), m_output_state.scale, m_output_state.scale);
    return {};
}

void WaylandClient::dispatch()
{
    MutexLocker locker(m_mutex);
    if (!m_display)
        return;
    if (wl_display_dispatch(m_display) >= 0) {
        // Requests queued while handling events (notably xdg_wm_base.pong) must be
        // flushed now; waiting for the next event would make the compositor think we
        // are unresponsive.
        wl_display_flush(m_display);
        return;
    }

    // The compositor is gone or rejected a request. wl_display_dispatch flushes
    // pending requests before it reads, so a compositor that closed the
    // connection shows up as EPIPE on the write while the protocol error it
    // queued first (an xdg-shell size error, say) is still unread. Read it
    // without flushing so the log names what was rejected instead of only EPIPE.
    auto error = wl_display_get_error(m_display);
    wl_interface const* interface = nullptr;
    uint32_t id = 0;
    auto code = wl_display_get_protocol_error(m_display, &interface, &id);

    if ((error == EPIPE || error == ECONNRESET) && wl_display_get_fd(m_display) >= 0) {
        struct pollfd pfd {
            wl_display_get_fd(m_display), POLLIN, 0
        };
        if (poll(&pfd, 1, 0) > 0 && wl_display_prepare_read(m_display) == 0) {
            if (wl_display_read_events(m_display) == 0)
                (void)wl_display_dispatch_pending(m_display);
            else
                wl_display_cancel_read(m_display);
            error = wl_display_get_error(m_display);
            interface = nullptr;
            id = 0;
            code = wl_display_get_protocol_error(m_display, &interface, &id);
        }
    }

    auto reason = code != 0
        ? ByteString::formatted("protocol error {} on {}#{}", code, interface ? interface->name : "object", id)
        : ByteString::formatted("the compositor closed the connection (errno {})", error);
    warnln("LibWM/Wayland: dispatch failed: {}", reason);

    // Reading a dead connection again would spin the notifier on an
    // always-readable fd, so stop it and ask the application to leave.
    if (m_notifier)
        m_notifier->set_enabled(false);
    if (m_connection_lost)
        m_connection_lost(reason);
    else
        Core::EventLoop::current().quit(0);
}

void WaylandClient::add_output(wl_output* output)
{
    MutexLocker locker(m_mutex);
    m_output_state.primary = output;
    m_output_state.outputs.append(output);
    wl_output_add_listener(output, &s_output_listener, this);
    if (m_output_state.xdg_manager) {
        auto* xdg_output = zxdg_output_manager_v1_get_xdg_output(m_output_state.xdg_manager, output);
        zxdg_output_v1_add_listener(xdg_output, &s_xdg_output_listener, this);
    }
}

void WaylandClient::set_xdg_output_manager(zxdg_output_manager_v1* manager)
{
    MutexLocker locker(m_mutex);
    m_output_state.xdg_manager = manager;
    for (auto* output : m_output_state.outputs) {
        auto* xdg_output = zxdg_output_manager_v1_get_xdg_output(manager, output);
        zxdg_output_v1_add_listener(xdg_output, &s_xdg_output_listener, this);
    }
}

void WaylandClient::add_wm_base(xdg_wm_base* wm_base)
{
    MutexLocker locker(m_mutex);
    m_wm_base = wm_base;
    xdg_wm_base_add_listener(wm_base, &s_wm_base_listener, this);
}

void WaylandClient::on_output_logical_size(wl_output*, Gfx::IntSize logical_size)
{
    MutexLocker locker(m_mutex);
    if (logical_size.is_empty())
        return;
    m_output_state.have_logical_size = true;
    m_output_state.screen_size = logical_size;
}

void WaylandClient::on_output_done()
{
    MutexLocker locker(m_mutex);
    // zxdg_output_v1 gives the real logical size under fractional scaling and
    // takes precedence over mode/scale.
    if (m_output_state.have_logical_size)
        return;
    if (m_output_state.physical_size.is_empty())
        return;
    m_output_state.screen_size = {
        m_output_state.physical_size.width() / m_output_state.scale,
        m_output_state.physical_size.height() / m_output_state.scale,
    };
}

// --- display link -------------------------------------------------------------

static void display_link_frame_done(void* data, wl_callback* callback, uint32_t)
{
    static_cast<WaylandClient*>(data)->on_display_link_frame_done(callback);
}

static wl_callback_listener const s_display_link_listener = {
    .done = display_link_frame_done,
};

void WaylandClient::enable_display_link()
{
    MutexLocker locker(m_mutex);
    m_display_link.enabled = true;
    request_display_link_frame();
}

void WaylandClient::disable_display_link()
{
    MutexLocker locker(m_mutex);
    m_display_link.enabled = false;
    if (m_display_link.callback) {
        wl_callback_destroy(m_display_link.callback);
        m_display_link.callback = nullptr;
        m_display_link.surface = nullptr;
    }
}

void WaylandClient::request_display_link_frame()
{
    MutexLocker locker(m_mutex);
    if (!m_display_link.enabled)
        return;
    // A mapped window surface fires frame_done once per refresh it is rendered in.
    // The most recently presented window is the one most likely to be animating.
    wl_surface* surface = nullptr;
    for (auto const& it : m_windows) {
        if (it.value->surface)
            surface = it.value->surface;
    }
    if (!surface)
        return;
    if (m_display_link.callback) {
        if (m_display_link.surface == surface)
            return;
        // That surface is gone or another is animating now: pace from the new one.
        wl_callback_destroy(m_display_link.callback);
        m_display_link.callback = nullptr;
    }
    m_display_link.surface = surface;
    m_display_link.callback = wl_surface_frame(surface);
    wl_callback_add_listener(m_display_link.callback, &s_display_link_listener, this);
}

void WaylandClient::on_display_link_frame_done(wl_callback* callback)
{
    MutexLocker locker(m_mutex);
    if (callback != m_display_link.callback) {
        wl_callback_destroy(callback);
        return;
    }
    wl_callback_destroy(callback);
    m_display_link.callback = nullptr;
    m_display_link.surface = nullptr;
    if (on_display_link)
        on_display_link();
    // Re-arm for the next refresh; the app's repaint in response is the damage
    // that makes the compositor render this surface again.
    request_display_link_frame();
}

}
