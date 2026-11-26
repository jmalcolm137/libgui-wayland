/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include "Environment.h"
#include "SerenityShell.h"
#include "WaylandDataDevice.h"
#include "WaylandOutputs.h"
#include "WaylandSeat.h"
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
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
        // Version 4 is where wl_keyboard.repeat_info appears; a lower seat
        // version means a compliant compositor sends no repeat info, and the
        // client cannot repeat held keys.
        self.seat().bind(reinterpret_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 4u))));
    else if (!strcmp(interface, wl_output_interface.name))
        self.outputs().add_output(reinterpret_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, min(version, 2u))));
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        self.add_wm_base(reinterpret_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1)));
    else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
        self.set_decoration_manager(reinterpret_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, min(version, 1u))));
    else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
        trace("LibWM/Wayland: found wl_data_device_manager v{}", version);
        self.data_device().bind(reinterpret_cast<wl_data_device_manager*>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, min(version, 3u))));
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        self.outputs().set_xdg_output_manager(reinterpret_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, min(version, 3u))));
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        self.set_viewporter(reinterpret_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1)));
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        self.set_fractional_scale_manager(reinterpret_cast<wp_fractional_scale_manager_v1*>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1)));
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        self.set_layer_shell(reinterpret_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, min(version, 4u))));
    } else if (!strcmp(interface, serenity_window_manager_interface.name)) {
        self.serenity_shell().bind(reinterpret_cast<serenity_window_manager*>(wl_registry_bind(registry, name, &serenity_window_manager_interface, min(version, 4u))));
    } else if (!strcmp(interface, zwp_text_input_manager_v3_interface.name)) {
        self.seat().set_text_input_manager(reinterpret_cast<zwp_text_input_manager_v3*>(wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1)));
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
    trace("LibWM/Wayland: xdg_wm_base ping serial={} -> pong", serial);
    xdg_wm_base_pong(base, serial);
}

static xdg_wm_base_listener const s_wm_base_listener = {
    .ping = wm_base_ping,
};

// --- connection & output lifecycle --------------------------------------------

WaylandClient::WaylandClient()
    : m_serenity_shell(make<SerenityShell>(*this))
    , m_outputs(make<WaylandOutputs>(*this))
    , m_data_device(make<WaylandDataDevice>(*this))
    , m_seat(make<WaylandSeat>(*this))
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

RefPtr<Gfx::Bitmap> copy_for_other_thread(Gfx::Bitmap const* bitmap)
{
    if (!bitmap)
        return nullptr;
    auto copy = bitmap->clone();
    return copy.is_error() ? nullptr : copy.release_value();
}

HashMap<String, ByteBuffer> copy_for_other_thread(HashMap<String, ByteBuffer> const& map)
{
    HashMap<String, ByteBuffer> copy;
    for (auto const& it : map) {
        auto bytes = ByteBuffer::copy(it.value.bytes());
        if (bytes.is_error())
            continue;
        copy.set(String::from_utf8_with_replacement_character(it.key.bytes_as_string_view()), bytes.release_value());
    }
    return copy;
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

    auto const screen = m_outputs->screen_size();
    trace("LibWM/Wayland: connected; screen {}x{}, output scale {}/120",
        screen.width(), screen.height(), m_outputs->output_scale_v120());
    return {};
}

bool WaylandClient::dispatch_until(Function<bool()> done, int timeout_ms)
{
    auto const deadline = MonotonicTime::now() + Duration::from_milliseconds(timeout_ms);
    for (;;) {
        {
            MutexLocker locker(m_mutex);
            if (done())
                return true;
            if (!m_display)
                return false;
            // The standard multi-reader sequence: safe alongside the main
            // thread's own dispatch, which takes the same mutex.
            while (wl_display_prepare_read(m_display) != 0)
                wl_display_dispatch_pending(m_display);
            wl_display_flush(m_display);
        }
        auto const remaining = (deadline - MonotonicTime::now()).to_milliseconds();
        struct pollfd pfd {
            wl_display_get_fd(m_display), POLLIN, 0
        };
        bool const readable = remaining > 0 && poll(&pfd, 1, static_cast<int>(min<i64>(remaining, 50))) > 0;
        MutexLocker locker(m_mutex);
        if (readable && wl_display_read_events(m_display) == 0)
            wl_display_dispatch_pending(m_display);
        else if (!readable)
            wl_display_cancel_read(m_display);
        if (done())
            return true;
        if (MonotonicTime::now() >= deadline)
            return false;
    }
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

void WaylandClient::add_wm_base(xdg_wm_base* wm_base)
{
    MutexLocker locker(m_mutex);
    m_wm_base = wm_base;
    xdg_wm_base_add_listener(wm_base, &s_wm_base_listener, this);
}

// --- display link -------------------------------------------------------------

static void display_link_frame_done(void* data, wl_callback* callback, uint32_t)
{
    static_cast<WaylandClient*>(data)->on_display_link_frame_done(callback);
}

static wl_callback_listener const s_display_link_listener = {
    .done = display_link_frame_done,
};

void WaylandClient::set_display_link_callback(void* owner, Function<void()> callback)
{
    MutexLocker locker(m_mutex);
    if (m_display_link.callback_owner)
        return;
    m_display_link.callback_owner = owner;
    m_display_link.callback_function = move(callback);
}

void WaylandClient::clear_display_link_callback(void* owner)
{
    MutexLocker locker(m_mutex);
    if (m_display_link.callback_owner != owner)
        return;
    m_display_link.callback_owner = nullptr;
    m_display_link.callback_function = nullptr;
}

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
    if (m_display_link.callback_function)
        m_display_link.callback_function();
    // Re-arm for the next refresh; the app's repaint in response is the damage
    // that makes the compositor render this surface again.
    request_display_link_frame();
}

}
