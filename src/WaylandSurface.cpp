/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include "SerenityShell.h"
#include "WaylandOutputs.h"
#include "Environment.h"
#include <AK/Assertions.h>
#include <LibCore/Process.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGUI/Window.h>
#include <WindowServer/WindowType.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace LibWM {

// libwayland speaks in messages that must fit its 4 KiB connection buffer.
// xdg_toplevel.set_title carries a string straight from page content, so an
// oversized title makes the server reset the connection. Clamp at a UTF-8
// boundary before handing strings to the protocol.
static ByteString clamp_wayland_string(ByteString const& value, size_t max_bytes = 1024)
{
    if (value.length() <= max_bytes)
        return value;

    size_t length = max_bytes;
    while (length > 0 && (static_cast<u8>(value[length]) & 0xc0) == 0x80)
        --length;
    return value.substring(0, length);
}

namespace {
using SurfaceLocker = WaylandClient::MutexLocker;
}

// The application id reported to the compositor (`xdg_toplevel.app_id`): the
// executable's base name, like an X11 WM_CLASS. The compositor uses it to group a
// window's Taskbar button with the other windows of the same application. It is
// the same for every window of this process.
static ByteString const& application_id()
{
    static ByteString s_application_id = [] {
        auto name = Core::Process::get_name().release_value_but_fixme_should_propagate_errors();
        auto view = name.bytes_as_string_view();
        if (auto slash = view.find_last('/'); slash.has_value())
            view = view.substring_view(*slash + 1);
        return ByteString { view };
    }();
    return s_application_id;
}

// --- xdg_surface_object -------------------------------------------------------

static void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    static_cast<WaylandClient*>(data)->on_surface_configured(surface);
}

static xdg_surface_listener const s_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// --- xdg_toplevel -------------------------------------------------------------

static void toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t width, int32_t height, wl_array* states)
{
    auto& self = *static_cast<WaylandClient*>(data);
    bool activated = false;
    bool fullscreen = false;
    bool maximized = false;
    bool resizing = false;
    auto* state_data = static_cast<uint32_t const*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        switch (state_data[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            activated = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            fullscreen = true;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            maximized = true;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            resizing = true;
            break;
        default:
            break;
        }
    }
    self.on_toplevel_configure(toplevel, { width, height }, activated, fullscreen, maximized, resizing);
}

static void toplevel_close(void* data, xdg_toplevel* toplevel)
{
    static_cast<WaylandClient*>(data)->on_toplevel_close(toplevel);
}

static void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) { }
static void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) { }

static xdg_toplevel_listener const s_toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

// --- zwlr_layer_surface_v1 (panels, the desktop, applets) ---------------------

static void layer_surface_configure(void* data, zwlr_layer_surface_v1* layer_surface, uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    static_cast<WaylandClient*>(data)->on_layer_configure(layer_surface, { static_cast<int>(width), static_cast<int>(height) });
}

static void layer_surface_closed(void* data, zwlr_layer_surface_v1* layer_surface)
{
    static_cast<WaylandClient*>(data)->on_layer_closed(layer_surface);
}

static zwlr_layer_surface_v1_listener const s_layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

// --- wp_fractional_scale_v1 ---------------------------------------------------

static void fractional_scale_preferred_scale(void* data, wp_fractional_scale_v1* fractional, uint32_t scale_v120)
{
    static_cast<WaylandClient*>(data)->on_preferred_scale(fractional, scale_v120);
}

static wp_fractional_scale_v1_listener const s_fractional_scale_listener = {
    .preferred_scale = fractional_scale_preferred_scale,
};

void WaylandClient::on_preferred_scale(wp_fractional_scale_v1* fractional, u32 scale_v120)
{
    MutexLocker locker(m_mutex);
    if (scale_v120 < 60)
        return;
    m_outputs->set_preferred_scale_v120(static_cast<int>(scale_v120));
    dbgln("LibWM/Wayland: preferred fractional scale {}/120 ({:.2f}x, backing {}x)",
        scale_v120, static_cast<double>(scale_v120) / 120.0, m_outputs->output_scale());

    if (Environment::client_scale_override_v120().has_value())
        return;

    int const previous_backing_scale = m_outputs->client_scale();
    m_outputs->set_client_scale_v120(static_cast<int>(scale_v120));
    int const backing_scale = m_outputs->client_scale();
    GUI::Window::set_backing_store_scale(backing_scale);

    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.fractional_scale != fractional)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        w.composed_valid = false;
        w.csd_dirty = true;
        w.inset_dirty = true;
        if (backing_scale != previous_backing_scale && m_input.window_resize)
            m_input.window_resize(w.window_id, w.size);
        return;
    }
}

// --- zxdg_toplevel_decoration_v1 ----------------------------------------------

static void decoration_configure(void* data, zxdg_toplevel_decoration_v1* decoration, uint32_t mode)
{
    dbgln("LibWM/Wayland: server-side decoration mode {}", mode);
    static_cast<WaylandClient*>(data)->on_decoration_configure(decoration, mode);
}

static zxdg_toplevel_decoration_v1_listener const s_decoration_listener = {
    .configure = decoration_configure,
};

// --- wl_buffer ----------------------------------------------------------------

static void buffer_release(void* data, wl_buffer*)
{
    static_cast<WaylandClient::BufferRecord*>(data)->released = true;
}

static wl_buffer_listener const s_buffer_listener = {
    .release = buffer_release,
};

// --- wl_callback (frame pacing) ----------------------------------------------

static void frame_done(void* data, wl_callback* callback, uint32_t)
{
    static_cast<WaylandClient*>(data)->on_frame_done(callback);
}

static wl_callback_listener const s_frame_listener = {
    .done = frame_done,
};

// --- surface & window lookups -------------------------------------------------

i32 WaylandClient::window_id_for_surface(wl_surface* surface) const
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        if (it.value->surface == surface || it.value->menubar_surface == surface)
            return it.value->window_id;
    }
    return -1;
}

WaylandClient::WindowSurface* WaylandClient::find(i32 window_id)
{
    MutexLocker locker(m_mutex);
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return nullptr;
    return it->value.ptr();
}

// --- configure & close handlers -----------------------------------------------

void WaylandClient::on_decoration_configure(zxdg_toplevel_decoration_v1* decoration, u32 mode)
{
    MutexLocker locker(m_mutex);
    bool const force_csd = Environment::force_csd();
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.decoration != decoration)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        bool const want_csd = !w.frameless && (force_csd || mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        if (w.use_csd != want_csd) {
            w.use_csd = want_csd;
            w.csd_dirty = true;
            w.composed_valid = false;
        }
        return;
    }
}

void WaylandClient::on_toplevel_configure(xdg_toplevel* toplevel, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized, bool resizing)
{
    MutexLocker locker(m_mutex);
    dbgln("LibWM/Wayland: toplevel configure {}x{} activated={} fullscreen={} maximized={} resizing={}", size.width(), size.height(), activated, fullscreen, maximized, resizing);
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.toplevel != toplevel)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        if (w.activated != activated) {
            w.activated = activated;
            w.csd_dirty = true;
        }
        if (m_input.window_activation)
            m_input.window_activation(w.window_id, activated);

        bool const was_fullscreen = w.fullscreen;
        w.fullscreen = fullscreen || maximized;
        w.maximized = maximized;

        if (!resizing)
            w.resizing = false;

        int const top_inset = w.total_top_inset();

        auto resize_to = [&](Gfx::IntSize content) {
            if (content == w.size)
                return;
            w.size = content;
            w.csd_dirty = true;
            if (m_input.window_resize)
                m_input.window_resize(w.window_id, content);
        };

        // Keep the client's last frame and let the compositor scale it to the
        // new size for the duration of an interactive resize. Returns false if
        // there is no viewport to stretch with.
        auto stretch_to = [&](Gfx::IntSize content) {
            if (!w.viewport)
                return false;
            wp_viewport_set_destination(w.viewport, content.width(), content.height() + top_inset);
            w.stretch_active = true;
            wl_surface_commit(w.surface);
            wl_display_flush(m_display);
            return true;
        };

        if (fullscreen || maximized) {
            // Fullscreen/maximize always resizes, even for non-resizable windows.
            if (size.width() > 0 && size.height() > top_inset)
                resize_to({ size.width(), size.height() - top_inset });
        } else if (was_fullscreen && !w.resizable) {
            // Leaving fullscreen: restore the fixed windowed size.
            resize_to(w.fixed_size);
        } else if (w.resizable) {
            // Interactive resize for resizable windows.
            if (size.width() > 0 && size.height() > top_inset) {
                auto content = Gfx::IntSize { size.width(), size.height() - top_inset };
                if (resizing) {
                    w.resizing = true;
                    // Don't re-render the client for every step; stretch instead.
                    if (!stretch_to(content))
                        resize_to(content);
                } else {
                    resize_to(content);
                }
            }
        }
        return;
    }
}

void WaylandClient::on_toplevel_close(xdg_toplevel* toplevel)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        if (it.value->toplevel == toplevel) {
            if (m_input.window_close_request)
                m_input.window_close_request(it.value->window_id);
            return;
        }
    }
}

void WaylandClient::on_layer_configure(zwlr_layer_surface_v1* layer_surface, Gfx::IntSize size)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.layer_surface != layer_surface)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        dbgln("LibWM/Wayland: layer configure {}x{}", size.width(), size.height());
        // Always deliver the first configure: the client's Window may still be at
        // its requested size (the Desktop is created zero-sized and sized by the
        // compositor), so skipping an equal-sized configure would leave it
        // unsized and the widget tree unlaid-out.
        bool const first_configure = !w.layer_configured;
        w.layer_configured = true;

        if (size.width() > 0 && size.height() > 0 && (first_configure || size != w.size)) {
            w.size = size;
            if (m_input.window_resize)
                m_input.window_resize(w.window_id, size);
        }

        // Flush a frame that arrived before we were allowed to attach.
        if (w.has_pending && w.pending_fd >= 0) {
            int fd = w.pending_fd;
            auto pending_size = w.pending_size;
            auto pending_visible_size = w.pending_visible_size;
            auto pending_pitch = w.pending_pitch;
            auto pending_has_alpha = w.pending_has_alpha;
            w.pending_fd = -1;
            w.has_pending = false;
            commit_window_content(w, fd, pending_size, pending_visible_size, pending_pitch, pending_has_alpha);
            ::close(fd);
        }
        return;
    }
}

void WaylandClient::on_layer_closed(zwlr_layer_surface_v1* layer_surface)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        if (it.value->layer_surface == layer_surface) {
            if (m_input.window_close_request)
                m_input.window_close_request(it.value->window_id);
            return;
        }
    }
}

void WaylandClient::on_surface_configured(xdg_surface* surface)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_popups) {
        auto& popup = *it.value;
        if (popup.xdg_surface_object != surface)
            continue;
        popup.configured = true;
        if (popup.pending) {
            bind_bitmap(popup.surface, popup.buffer, *popup.bitmap, popup.buffers);
            wl_surface_commit(popup.surface);
            wl_display_flush(m_display);
            popup.pending = nullptr;
        }
        return;
    }

    // The first configure on an xdg_toplevel: it is now safe to attach a buffer,
    // so flush a frame that arrived before we were allowed to.
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.xdg_surface_object != surface)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        w.toplevel_configured = true;
        if (w.has_pending && w.pending_fd >= 0) {
            int fd = w.pending_fd;
            auto pending_size = w.pending_size;
            auto pending_visible_size = w.pending_visible_size;
            auto pending_pitch = w.pending_pitch;
            auto pending_has_alpha = w.pending_has_alpha;
            w.pending_fd = -1;
            w.has_pending = false;
            commit_window_content(w, fd, pending_size, pending_visible_size, pending_pitch, pending_has_alpha);
            ::close(fd);
        }
        return;
    }
}

// --- buffer management & damage -----------------------------------------------

void WaylandClient::purge_released_buffers(Vector<NonnullOwnPtr<BufferRecord>>& buffers)
{
    Vector<NonnullOwnPtr<BufferRecord>> kept;
    for (auto& record : buffers) {
        if (record->released)
            wl_buffer_destroy(record->buffer);
        else
            kept.append(move(record));
    }
    buffers = move(kept);
}

void WaylandClient::purge_released_buffers(WindowSurface& window_surface)
{
    purge_released_buffers(window_surface.buffers);
    purge_released_buffers(window_surface.menubar_buffers);
}

void WaylandClient::bind_bitmap(wl_surface* surface, Core::AnonymousBuffer const& buffer, Gfx::Bitmap const& bitmap, Vector<NonnullOwnPtr<BufferRecord>>& buffers, Vector<Gfx::IntRect> const* damage)
{
    MutexLocker locker(m_mutex);
    if (!m_shm)
        return;
    int fd = dup(buffer.fd());
    if (fd < 0)
        return;
    int stride = bitmap.pitch();
    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int>(stride) * bitmap.physical_height());
    if (!pool) {
        close(fd);
        return;
    }
    auto format = bitmap.format() == Gfx::BitmapFormat::BGRA8888 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    auto* wl_buf = wl_shm_pool_create_buffer(pool, 0, bitmap.physical_width(), bitmap.physical_height(), stride, format);
    if (!wl_buf) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }
    // A HiDPI-scaled bitmap carries its scale; present the physical buffer and let
    // the compositor map it back to logical size via buffer_scale.
    if (bitmap.scale() > 1)
        wl_surface_set_buffer_scale(surface, bitmap.scale());
    wl_surface_attach(surface, wl_buf, 0, 0);
    if (damage && !damage->is_empty()) {
        for (auto const& rect : *damage)
            wl_surface_damage(surface, rect.x(), rect.y(), rect.width(), rect.height());
    } else {
        wl_surface_damage(surface, 0, 0, bitmap.width(), bitmap.height());
    }
    // Flush so the pool's fd is transferred before we close our copy.
    wl_display_flush(m_display);
    wl_shm_pool_destroy(pool);
    close(fd);

    auto record = make<BufferRecord>();
    record->buffer = wl_buf;
    wl_buffer_add_listener(wl_buf, &s_buffer_listener, record.ptr());
    buffers.append(move(record));
}

void WaylandClient::add_window_damage(i32 window_id, Vector<Gfx::IntRect> const& rects)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface)
        return;
    SurfaceLocker surface_locker(window_surface->mutex);
    if (window_surface->damage_full)
        return;
    // Coalesce: too many rects (or a window that never presents) collapses to a
    // full repaint rather than growing without bound.
    if (window_surface->damage.size() + rects.size() > 64) {
        window_surface->damage.clear();
        window_surface->damage_full = true;
        return;
    }
    for (auto const& rect : rects)
        window_surface->damage.append(rect);
}

// --- window lifecycle & presentation ------------------------------------------

void WaylandClient::create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize size, ByteString const& title, bool has_alpha, bool resizable, bool frameless, i32 window_type, i32 window_mode, i32 parent_window_id, Gfx::IntRect const& launch_origin)
{
    MutexLocker locker(m_mutex);
    if (!m_compositor || !m_wm_base)
        return;

    // Serenity shell window types map directly onto wlr-layer-shell roles: the
    // Taskbar is a bottom panel, the Desktop is a background surface, and
    // applets are top-layer surfaces. This replaces the earlier app_id hint.
    auto type = static_cast<WindowServer::WindowType>(window_type);
    if (m_layer_shell && (type == WindowServer::WindowType::Taskbar || type == WindowServer::WindowType::Desktop || type == WindowServer::WindowType::Applet)) {
        uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        uint32_t anchor = 0;
        uint32_t width = size.width();
        uint32_t height = size.height();
        int exclusive_zone = 0;
        char const* layer_namespace = "serenity-window";
        bool is_panel = false;
        // Applets encode their name in the namespace so the compositor can order
        // them ([Applet] Order) and lay them out (WS4).
        ByteString applet_namespace;

        switch (type) {
        case WindowServer::WindowType::Taskbar:
            // A bottom panel: stretch across the output and reserve its height.
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            width = 0;
            exclusive_zone = size.height();
            layer_namespace = "serenity-taskbar";
            is_panel = true;
            break;
        case WindowServer::WindowType::Desktop:
            // A full-screen background surface behind everything else.
            layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            width = 0;
            height = 0;
            exclusive_zone = -1;
            layer_namespace = "serenity-desktop";
            break;
        case WindowServer::WindowType::Applet:
            // Applets sit in the top layer, anchored to the bottom-right. The
            // compositor overrides the position, laying them out in the Taskbar's
            // applet area; the namespace carries the applet's name.
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            applet_namespace = ByteString::formatted("serenity-applet:{}", title);
            layer_namespace = applet_namespace.characters();
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        auto* layer_surface_object = wl_compositor_create_surface(m_compositor);
        auto* layer_surface = zwlr_layer_shell_v1_get_layer_surface(m_layer_shell, layer_surface_object, nullptr, layer, layer_namespace);
        zwlr_layer_surface_v1_add_listener(layer_surface, &s_layer_surface_listener, this);
        zwlr_layer_surface_v1_set_size(layer_surface, width, height);
        zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, exclusive_zone);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(layer_surface_object);

        auto window_surface = make<WindowSurface>();
        window_surface->window_id = window_id;
        window_surface->surface = layer_surface_object;
        window_surface->layer_surface = layer_surface;
        window_surface->is_layer = true;
        window_surface->layer_panel = is_panel;
        window_surface->requested_position = position;
        switch (type) {
        case WindowServer::WindowType::Desktop:
            window_surface->layer_output_position = { 0, 0 };
            break;
        case WindowServer::WindowType::Taskbar:
            window_surface->layer_output_position = { 0, m_outputs->screen_size().height() - size.height() };
            break;
        case WindowServer::WindowType::Applet:
            window_surface->layer_output_position = { m_outputs->screen_size().width() - size.width(), m_outputs->screen_size().height() - size.height() };
            break;
        default:
            break;
        }
        window_surface->title = title;
        window_surface->size = size;
        window_surface->has_alpha = has_alpha;
        window_surface->resizable = resizable;
        window_surface->fixed_size = size;
        if (m_viewporter)
            window_surface->viewport = wp_viewporter_get_viewport(m_viewporter, layer_surface_object);

        // Let the compositor report this layer surface's real output position:
        // the applet area moves applets away from their anchored corner, so the
        // client's own origin would otherwise be stale.
        window_surface->chrome = m_serenity_shell->create_toplevel(layer_surface_object);

        m_windows.set(window_id, move(window_surface));
        wl_display_flush(m_display);
        return;
    }

    // Popup windows (ComboBox dropdowns, autocomplete, tooltips, menus opened as
    // windows) are frameless overlays the client positions itself.
    if (m_layer_shell && (type == WindowServer::WindowType::Popup || type == WindowServer::WindowType::Autocomplete || type == WindowServer::WindowType::Tooltip || type == WindowServer::WindowType::Menu || type == WindowServer::WindowType::Notification)) {
        bool const is_notification = type == WindowServer::WindowType::Notification;
        bool const is_tooltip = type == WindowServer::WindowType::Tooltip;
        bool const takes_focus = !is_notification && !is_tooltip;
        char const* layer_namespace = is_notification ? "serenity-notification"
            : is_tooltip                              ? "serenity-tooltip"
                                                      : "serenity-popup";
        auto* surface = wl_compositor_create_surface(m_compositor);
        auto* layer_surface = zwlr_layer_shell_v1_get_layer_surface(m_layer_shell, surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, layer_namespace);
        zwlr_layer_surface_v1_add_listener(layer_surface, &s_layer_surface_listener, this);
        zwlr_layer_surface_v1_set_size(layer_surface, size.width(), size.height());
        zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, takes_focus ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        zwlr_layer_surface_v1_set_margin(layer_surface, position.y(), 0, 0, position.x());
        wl_surface_commit(surface);

        auto window_surface = make<WindowSurface>();
        window_surface->window_id = window_id;
        window_surface->surface = surface;
        window_surface->layer_surface = layer_surface;
        window_surface->is_layer = true;
        window_surface->requested_position = position;
        window_surface->layer_output_position = position;
        window_surface->title = title;
        window_surface->size = size;
        window_surface->has_alpha = has_alpha;
        window_surface->resizable = resizable;
        window_surface->fixed_size = size;
        if (m_viewporter)
            window_surface->viewport = wp_viewporter_get_viewport(m_viewporter, surface);

        m_windows.set(window_id, move(window_surface));
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: popup window {} ({}x{}) at {},{}", window_id, size.width(), size.height(), position.x(), position.y());
        return;
    }

    auto* surface = wl_compositor_create_surface(m_compositor);
    auto* xdg_surface_object = xdg_wm_base_get_xdg_surface(m_wm_base, surface);
    xdg_surface_add_listener(xdg_surface_object, &s_xdg_surface_listener, this);
    auto* toplevel = xdg_surface_get_toplevel(xdg_surface_object);
    xdg_toplevel_add_listener(toplevel, &s_toplevel_listener, this);
    auto clamped_title = clamp_wayland_string(title);
    xdg_toplevel_set_title(toplevel, clamped_title.characters());
    xdg_toplevel_set_app_id(toplevel, application_id().characters());
    // A child window (a dialog) sets its transient parent, so the compositor can
    // keep it above its parent and track the modal chain.
    if (parent_window_id >= 0) {
        if (auto it = m_windows.find(parent_window_id); it != m_windows.end() && it->value->toplevel)
            xdg_toplevel_set_parent(toplevel, it->value->toplevel);
    }
    wl_surface_commit(surface);

    auto window_surface = make<WindowSurface>();
    window_surface->window_id = window_id;
    window_surface->surface = surface;
    window_surface->xdg_surface_object = xdg_surface_object;
    window_surface->toplevel = toplevel;
    window_surface->title = title;
    window_surface->size = size;
    window_surface->has_alpha = has_alpha;
    window_surface->resizable = resizable;
    window_surface->frameless = frameless;
    window_surface->fixed_size = size;
    bool const force_csd = Environment::force_csd();
    window_surface->use_csd = !frameless && (!m_decoration_manager || force_csd);
    if (window_surface->use_csd)
        dbgln("LibWM/Wayland: using client-side decorations for window {}", window_id);
    if (m_viewporter)
        window_surface->viewport = wp_viewporter_get_viewport(m_viewporter, surface);
    if (m_fractional_scale_manager) {
        window_surface->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(m_fractional_scale_manager, surface);
        if (window_surface->fractional_scale)
            wp_fractional_scale_v1_add_listener(window_surface->fractional_scale, &s_fractional_scale_listener, this);
    }

    // Let the compositor drive this window's chrome (the menu bar).
    window_surface->chrome = m_serenity_shell->create_toplevel(surface, window_mode, launch_origin);

    // A non-resizable window pins min == max (fullscreen still overrides this).
    if (!resizable) {
        int const h = size.height() + window_surface->total_top_inset();
        xdg_toplevel_set_min_size(toplevel, size.width(), h);
        xdg_toplevel_set_max_size(toplevel, size.width(), h);
    }

    // Ask the compositor to draw server-side decorations (titlebar, buttons,
    // and therefore a draggable frame) unless client-side decorations are forced.
    if (m_decoration_manager && !force_csd) {
        auto* decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(m_decoration_manager, toplevel);
        zxdg_toplevel_decoration_v1_add_listener(decoration, &s_decoration_listener, this);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        window_surface->decoration = decoration;
    }

    m_windows.set(window_id, move(window_surface));
    wl_display_flush(m_display);
}

void WaylandClient::destroy_window(i32 window_id)
{
    MutexLocker locker(m_mutex);
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return;
    auto& window_surface = *it->value;
    {
        SurfaceLocker surface_locker(window_surface.mutex);
        if (window_surface.frame_callback)
            wl_callback_destroy(window_surface.frame_callback);
        window_surface.frame_callback = nullptr;
        window_surface.frame_in_flight = false;
        if (window_surface.pending_fd >= 0)
            ::close(window_surface.pending_fd);
        window_surface.pending_fd = -1;
        window_surface.has_pending = false;
        for (auto& record : window_surface.buffers) {
            if (record->buffer)
                wl_buffer_destroy(record->buffer);
        }
        if (window_surface.layer_surface)
            zwlr_layer_surface_v1_destroy(window_surface.layer_surface);
        m_serenity_shell->destroy_toplevel(window_surface.chrome);
        if (window_surface.decoration)
            zxdg_toplevel_decoration_v1_destroy(window_surface.decoration);
        if (window_surface.toplevel)
            xdg_toplevel_destroy(window_surface.toplevel);
        if (window_surface.fractional_scale)
            wp_fractional_scale_v1_destroy(window_surface.fractional_scale);
        if (window_surface.viewport)
            wp_viewport_destroy(window_surface.viewport);
        if (window_surface.xdg_surface_object)
            xdg_surface_destroy(window_surface.xdg_surface_object);
        if (m_display_link.surface == window_surface.surface) {
            if (m_display_link.callback)
                wl_callback_destroy(m_display_link.callback);
            m_display_link.callback = nullptr;
            m_display_link.surface = nullptr;
        }
        if (m_input_state.pointer_surface == window_surface.surface || m_input_state.pointer_surface == window_surface.menubar_surface)
            m_input_state.pointer_surface = nullptr;
        if (m_input_state.pointer_window == window_id)
            m_input_state.pointer_window = -1;
        if (m_input_state.focused_window == window_id)
            m_input_state.focused_window = -1;
        for (auto& record : window_surface.menubar_buffers) {
            if (record->buffer)
                wl_buffer_destroy(record->buffer);
        }
        window_surface.menubar_buffers.clear();
        if (window_surface.menubar_subsurface) {
            wl_subsurface_destroy(window_surface.menubar_subsurface);
            window_surface.menubar_subsurface = nullptr;
        }
        if (window_surface.menubar_surface) {
            wl_surface_destroy(window_surface.menubar_surface);
            window_surface.menubar_surface = nullptr;
        }
        if (window_surface.surface)
            wl_surface_destroy(window_surface.surface);
    }
    m_windows.remove(it);
    request_display_link_frame();
}

void WaylandClient::set_title(i32 window_id, ByteString const& title)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        SurfaceLocker surface_locker(window_surface->mutex);
        auto clamped = clamp_wayland_string(title);
        window_surface->title = clamped;
        window_surface->csd_dirty = true;
        if (window_surface->toplevel)
            xdg_toplevel_set_title(window_surface->toplevel, clamped.characters());
        wl_display_flush(m_display);
    }
}

void WaylandClient::set_window_rect(i32 window_id, Gfx::IntSize size)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface || !window_surface->is_layer || !window_surface->layer_surface)
        return;
    SurfaceLocker surface_locker(window_surface->mutex);

    if (window_surface->layer_panel) {
        if (size.height() == window_surface->size.height())
            return;
        zwlr_layer_surface_v1_set_size(window_surface->layer_surface, 0, size.height());
        zwlr_layer_surface_v1_set_exclusive_zone(window_surface->layer_surface, size.height());
    } else {
        if (size == window_surface->size)
            return;
        zwlr_layer_surface_v1_set_size(window_surface->layer_surface, size.width(), size.height());
    }
    window_surface->size = size;
    wl_surface_commit(window_surface->surface);
    wl_display_flush(m_display);
}

void WaylandClient::set_fullscreen(i32 window_id, bool fullscreen)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        if (!window_surface->toplevel)
            return;
        if (fullscreen)
            xdg_toplevel_set_fullscreen(window_surface->toplevel, nullptr);
        else
            xdg_toplevel_unset_fullscreen(window_surface->toplevel);
        wl_display_flush(m_display);
    }
}

void WaylandClient::set_maximized(i32 window_id, bool maximized)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        window_surface->maximized = maximized;
        if (!window_surface->toplevel)
            return;
        if (maximized)
            xdg_toplevel_set_maximized(window_surface->toplevel);
        else
            xdg_toplevel_unset_maximized(window_surface->toplevel);
        wl_display_flush(m_display);
    }
}

bool WaylandClient::is_window_maximized(i32 window_id) const
{
    MutexLocker locker(m_mutex);
    auto it = m_windows.find(window_id);
    return it != m_windows.end() && it->value->maximized;
}

void WaylandClient::set_minimum_size(i32 window_id, Gfx::IntSize size)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        SurfaceLocker surface_locker(window_surface->mutex);
        window_surface->minimum_size = size;
        if (window_surface->toplevel && window_surface->resizable) {
            int const min_w = max(0, size.width());
            int const min_h = size.height() > 0 ? size.height() + window_surface->total_top_inset() : 0;
            xdg_toplevel_set_min_size(window_surface->toplevel, min_w, min_h);
            wl_display_flush(m_display);
            dbgln("LibWM/Wayland: set_min_size window {} -> {}x{}", window_id, min_w, min_h);
        }
    }
}

void WaylandClient::set_frameless(i32 window_id, bool frameless)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        SurfaceLocker surface_locker(window_surface->mutex);
        if (window_surface->frameless == frameless)
            return;
        window_surface->frameless = frameless;
        bool const force_csd = Environment::force_csd();
        window_surface->use_csd = !frameless && (!m_decoration_manager || force_csd);
        window_surface->csd_dirty = true;
        window_surface->composed_valid = false;
    }
}

void WaylandClient::set_window_has_alpha_channel(i32 window_id, bool has_alpha)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        SurfaceLocker surface_locker(window_surface->mutex);
        window_surface->has_alpha = has_alpha;
        window_surface->composed_valid = false;
    }
}

void WaylandClient::set_window_alpha_hit_threshold(i32 window_id, float threshold)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        SurfaceLocker surface_locker(window_surface->mutex);
        window_surface->alpha_hit_threshold = clamp(threshold, 0.0f, 1.0f);
    }
}

void WaylandClient::set_minimized(i32 window_id)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id)) {
        if (!window_surface->toplevel)
            return;
        xdg_toplevel_set_minimized(window_surface->toplevel);
        wl_display_flush(m_display);
    }
}

void WaylandClient::set_always_on_top(i32 window_id, bool always_on_top)
{
    MutexLocker locker(m_mutex);
    if (auto* window_surface = find(window_id))
        m_serenity_shell->set_always_on_top(window_surface->chrome, always_on_top);
}

void WaylandClient::set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const& icon)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface)
        return;
    {
        SurfaceLocker surface_locker(window_surface->mutex);
        window_surface->icon_bitmap = icon.is_valid() ? icon.bitmap() : nullptr;
        window_surface->csd_dirty = true;
    }
    m_serenity_shell->set_icon(window_surface->chrome, window_surface->icon_bitmap.ptr());
}

void WaylandClient::attach_and_commit(i32 window_id, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface || !m_shm || size.is_empty())
        return;
    SurfaceLocker surface_locker(window_surface->mutex);
    if (visible_size.is_empty())
        visible_size = size;

    // A layer surface or an xdg_toplevel must not attach a buffer before its
    // first configure; hold the frame and present it from the configure handler.
    bool const awaiting_first_configure = (window_surface->is_layer && !window_surface->layer_configured)
        || (window_surface->toplevel && !window_surface->toplevel_configured);
    if (awaiting_first_configure) {
        if (window_surface->pending_fd >= 0)
            ::close(window_surface->pending_fd);
        window_surface->pending_fd = ::dup(client_fd);
        if (window_surface->pending_fd < 0)
            return;
        window_surface->pending_size = size;
        window_surface->pending_visible_size = visible_size;
        window_surface->pending_pitch = pitch;
        window_surface->pending_has_alpha = has_alpha;
        window_surface->has_pending = true;
        return;
    }

    // Pace presents to the compositor: if a frame is still in flight, keep only
    // the newest content and attach it when the frame callback lands.
    if (window_surface->frame_in_flight) {
        if (window_surface->pending_fd >= 0)
            ::close(window_surface->pending_fd);
        window_surface->pending_fd = ::dup(client_fd);
        if (window_surface->pending_fd < 0)
            return;
        window_surface->pending_size = size;
        window_surface->pending_visible_size = visible_size;
        window_surface->pending_pitch = pitch;
        window_surface->pending_has_alpha = has_alpha;
        window_surface->has_pending = true;
        return;
    }

    commit_window_content(*window_surface, client_fd, size, visible_size, pitch, has_alpha);
}

void WaylandClient::render_csd_titlebar(WindowSurface const& window_surface, Gfx::Bitmap& destination, int width)
{
    int const csd_h = window_surface.csd_height();
    if (csd_h <= 0 || width <= 0)
        return;

    Gfx::Painter painter(destination);
    Gfx::Palette palette { Gfx::PaletteImpl::create_with_anonymous_buffer(Gfx::current_system_theme_buffer()) };

    Gfx::IntRect const bar_rect { 0, 0, width, csd_h };
    Gfx::StylePainter::paint_frame(painter, bar_rect, palette, Gfx::FrameStyle::RaisedPanel);

    Gfx::IntRect const inner { 2, 2, max(0, width - 4), max(0, csd_h - 4) };
    auto const c1 = window_surface.activated ? palette.active_window_border1() : palette.inactive_window_border1();
    auto const c2 = window_surface.activated ? palette.active_window_border2() : palette.inactive_window_border2();
    auto const text_color = window_surface.activated ? palette.active_window_title() : palette.inactive_window_title();
    painter.fill_rect_with_gradient(Gfx::Orientation::Horizontal, inner, c1, c2);

    int text_x = 6;
    if (window_surface.icon_bitmap) {
        Gfx::IntRect const icon_rect { 4, (csd_h - 16) / 2, 16, 16 };
        painter.draw_scaled_bitmap(icon_rect, *window_surface.icon_bitmap, window_surface.icon_bitmap->rect());
        text_x = 24;
    }

    Gfx::IntRect const close_rect { width - 20, 4, 16, 14 };
    Gfx::IntRect const max_rect { width - 38, 4, 16, 14 };
    Gfx::IntRect const min_rect { width - (window_surface.resizable ? 56 : 38), 4, 16, 14 };
    int const buttons_left = min_rect.x();

    if (buttons_left > text_x + 4) {
        Gfx::IntRect const title_rect { text_x, 2, buttons_left - text_x - 4, csd_h - 4 };
        painter.draw_text(title_rect, window_surface.title, Gfx::FontDatabase::window_title_font(), Gfx::TextAlignment::CenterLeft, text_color, Gfx::TextElision::Right);
    }

    auto paint_caption_button = [&](Gfx::IntRect const& rect, StringView glyph) {
        if (rect.x() < 2)
            return;
        Gfx::StylePainter::paint_button(painter, rect, palette, Gfx::ButtonStyle::Normal, false);
        painter.draw_text(rect, glyph, Gfx::FontDatabase::window_title_font(), Gfx::TextAlignment::Center, palette.button_text());
    };
    paint_caption_button(min_rect, "_"sv);
    if (window_surface.resizable)
        paint_caption_button(max_rect, "\xE2\x96\xA1"sv);
    paint_caption_button(close_rect, "\xC3\x97"sv);
}

static void resample_to_fractional(u8 const* src_base, size_t src_pitch, int src_w, int src_h, Gfx::Bitmap& dst)
{
    int const dst_w = dst.physical_width();
    int const dst_h = dst.physical_height();
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
        return;

    for (int dy = 0; dy < dst_h; ++dy) {
        int const sy = min(src_h - 1, ((2 * dy + 1) * src_h) / (2 * dst_h));
        auto const* src_row = reinterpret_cast<u32 const*>(src_base + static_cast<size_t>(sy) * src_pitch);
        auto* dst_row = dst.scanline(dy);
        for (int dx = 0; dx < dst_w; ++dx) {
            int const sx = min(src_w - 1, ((2 * dx + 1) * src_w) / (2 * dst_w));
            dst_row[dx] = src_row[sx];
        }
    }
}

// Opaque region hint: lets the compositor skip blending for opaque windows. The
// region only depends on the logical size and alpha flag, so it is sent only when
// one of those changes (it is double-buffered state that persists across commits).
void WaylandClient::update_opaque_region(WindowSurface& window_surface, Gfx::IntSize logical_size)
{
    if (!m_compositor || !window_surface.surface)
        return;

    Gfx::IntSize const wanted = window_surface.has_alpha ? Gfx::IntSize {} : logical_size;
    if (window_surface.opaque_region_size == wanted)
        return;
    window_surface.opaque_region_size = wanted;

    if (wanted.is_empty()) {
        wl_surface_set_opaque_region(window_surface.surface, nullptr);
        return;
    }
    if (auto* opaque = wl_compositor_create_region(m_compositor)) {
        wl_region_add(opaque, 0, 0, wanted.width(), wanted.height());
        wl_surface_set_opaque_region(window_surface.surface, opaque);
        wl_region_destroy(opaque);
        dbgln("LibWM/Wayland: set opaque region on window {} ({}x{})", window_surface.window_id, wanted.width(), wanted.height());
    }
}

bool WaylandClient::needs_input_region_pixels(WindowSurface const& window_surface)
{
    return (window_surface.has_alpha && window_surface.alpha_hit_threshold > 0.0f) || window_surface.input_region_active;
}

// Alpha click-through input region: mask out pixels with alpha < threshold.
void WaylandClient::update_input_region(WindowSurface& window_surface, u8 const* client_pixels, size_t pitch, Gfx::IntSize visible_size, int scale, int total_inset)
{
    if (!m_compositor || !window_surface.surface)
        return;

    int const logical_w = visible_size.width();

    if (window_surface.has_alpha && window_surface.alpha_hit_threshold > 0.0f && client_pixels != nullptr) {
        auto* input_region = wl_compositor_create_region(m_compositor);
        if (!input_region)
            return;
        if (total_inset > 0)
            wl_region_add(input_region, 0, 0, logical_w, total_inset);

        u8 const min_alpha = static_cast<u8>(clamp(window_surface.alpha_hit_threshold * 255.0f, 1.0f, 255.0f));
        size_t span_count = 0;
        for (int y = 0; y < visible_size.height(); ++y) {
            auto const* row = reinterpret_cast<u32 const*>(client_pixels + static_cast<size_t>(y * scale) * pitch);
            int run_start = -1;
            for (int x = 0; x < logical_w; ++x) {
                u8 const alpha = static_cast<u8>((row[x * scale] >> 24) & 0xff);
                if (alpha >= min_alpha) {
                    if (run_start < 0)
                        run_start = x;
                } else if (run_start >= 0) {
                    wl_region_add(input_region, run_start, y + total_inset, x - run_start, 1);
                    ++span_count;
                    run_start = -1;
                }
            }
            if (run_start >= 0) {
                wl_region_add(input_region, run_start, y + total_inset, logical_w - run_start, 1);
                ++span_count;
            }
        }
        wl_surface_set_input_region(window_surface.surface, input_region);
        wl_region_destroy(input_region);
        window_surface.input_region_active = true;
        dbgln("LibWM/Wayland: applied alpha input region on window {} (threshold={:.2f}, spans={})",
            window_surface.window_id, window_surface.alpha_hit_threshold, span_count);
    } else if (window_surface.input_region_active) {
        wl_surface_set_input_region(window_surface.surface, nullptr);
        window_surface.input_region_active = false;
    }
}

void WaylandClient::commit_window_content(WindowSurface& window_surface, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha)
{
    MutexLocker locker(m_mutex);
    if (m_display_link.enabled)
        request_display_link_frame();
    auto request_frame = [&] {
        window_surface.frame_callback = wl_surface_frame(window_surface.surface);
        wl_callback_add_listener(window_surface.frame_callback, &s_frame_listener, this);
        window_surface.frame_in_flight = true;
    };

    int const scale = m_outputs->client_scale();
    int const scale_v120 = m_outputs->client_scale_v120();
    bool const is_fractional = (scale_v120 % 120) != 0;

    if (window_surface.stretch_active && !window_surface.resizing) {
        if (window_surface.viewport && !is_fractional)
            wp_viewport_set_destination(window_surface.viewport, -1, -1);
        window_surface.stretch_active = false;
    }

    purge_released_buffers(window_surface);

    Vector<Gfx::IntRect> damage = move(window_surface.damage);
    window_surface.damage.clear();
    bool const full_damage = window_surface.damage_full || damage.is_empty();
    window_surface.damage_full = false;

    if (window_surface.menubar_surface) {
        present_menubar(window_surface, visible_size);
        if (window_surface.xdg_surface_object)
            xdg_surface_set_window_geometry(window_surface.xdg_surface_object, 0, 0,
                visible_size.width(), visible_size.height());
    }

    int const csd_h = window_surface.csd_height();
    int const menubar_h = (window_surface.inset > 0 && window_surface.draw_inset && !window_surface.menubar_surface) ? window_surface.inset : 0;
    int const total_inset = csd_h + menubar_h;
    int const logical_height = visible_size.height() + total_inset;

    auto present_fractional = [&](u8 const* src_base, size_t src_pitch, int src_w, int src_h) -> bool {
        int const frac_w = max(1, (visible_size.width() * scale_v120 + 60) / 120);
        int const frac_h = max(1, (logical_height * scale_v120 + 60) / 120);
        auto const fmt = has_alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
        size_t const frac_pitch = Gfx::Bitmap::minimum_pitch(frac_w, fmt);
        if (!window_surface.fractional_bitmap || window_surface.fractional_bitmap->size() != Gfx::IntSize { frac_w, frac_h } || window_surface.fractional_bitmap->format() != fmt) {
            auto buf = Core::AnonymousBuffer::create_with_size(frac_pitch * static_cast<size_t>(frac_h));
            if (buf.is_error())
                return false;
            window_surface.fractional_buffer = buf.release_value();
            auto bmp = Gfx::Bitmap::create_with_anonymous_buffer(fmt, window_surface.fractional_buffer, { frac_w, frac_h }, 1);
            if (bmp.is_error())
                return false;
            window_surface.fractional_bitmap = bmp.release_value();
        }
        resample_to_fractional(src_base, src_pitch, src_w, src_h, *window_surface.fractional_bitmap);
        wl_surface_set_buffer_scale(window_surface.surface, 1);
        if (window_surface.viewport && !window_surface.resizing)
            wp_viewport_set_destination(window_surface.viewport, visible_size.width(), logical_height);
        Vector<Gfx::IntRect> frac_damage;
        frac_damage.append({ 0, 0, visible_size.width(), logical_height });
        request_frame();
        bind_bitmap(window_surface.surface, window_surface.fractional_buffer, *window_surface.fractional_bitmap, window_surface.buffers, &frac_damage);
        wl_surface_commit(window_surface.surface);
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: presented window {} ({}x{} @{:.2f}x -> {}x{}px incl. {}px csd + {}px inset)",
            window_surface.window_id, visible_size.width(), visible_size.height(),
            static_cast<double>(scale_v120) / 120.0, frac_w, frac_h, csd_h, menubar_h);
        return true;
    };

    if (total_inset > 0) {
        int const display_width = visible_size.width() * scale;
        int const display_height = logical_height * scale;
        size_t const composed_pitch = Gfx::Bitmap::minimum_pitch(display_width, Gfx::BitmapFormat::BGRA8888);
        size_t const composed_height = static_cast<size_t>(display_height);

        bool recreate = !window_surface.composed_valid || !window_surface.composed_bitmap
            || window_surface.composed_bitmap->size() != Gfx::IntSize { visible_size.width(), logical_height }
            || window_surface.composed_bitmap->scale() != scale;
        if (recreate) {
            auto buffer = Core::AnonymousBuffer::create_with_size(composed_pitch * composed_height);
            if (buffer.is_error())
                return;
            window_surface.composed_buffer = buffer.release_value();
            auto composed = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, window_surface.composed_buffer, { visible_size.width(), logical_height }, scale);
            if (composed.is_error())
                return;
            window_surface.composed_bitmap = composed.release_value();
            window_surface.composed_valid = true;
        }
        auto& destination = *window_surface.composed_bitmap;

        size_t client_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(size.height());
        auto* source = static_cast<u8 const*>(mmap(nullptr, client_bytes, PROT_READ, MAP_SHARED, client_fd, 0));
        if (source == MAP_FAILED)
            return;
        auto copy_rect = [&](Gfx::IntRect rect) {
            rect = rect.intersected({ 0, 0, visible_size.width(), visible_size.height() });
            if (rect.is_empty())
                return;
            size_t const row_bytes = static_cast<size_t>(rect.width()) * scale * 4;
            for (int row = rect.y() * scale; row < (rect.y() + rect.height()) * scale; ++row)
                memcpy(destination.scanline(total_inset * scale + row) + static_cast<size_t>(rect.x()) * scale,
                    source + static_cast<size_t>(row) * pitch + static_cast<size_t>(rect.x()) * scale * 4,
                    row_bytes);
        };
        if (recreate || full_damage) {
            copy_rect({ 0, 0, visible_size.width(), visible_size.height() });
        } else {
            for (auto const& rect : damage)
                copy_rect(rect);
        }
        if (needs_input_region_pixels(window_surface))
            update_input_region(window_surface, source, static_cast<size_t>(pitch), visible_size, scale, total_inset);
        munmap(const_cast<u8*>(source), client_bytes);
        update_opaque_region(window_surface, { visible_size.width(), logical_height });

        bool const redraw_csd = csd_h > 0 && (recreate || window_surface.csd_dirty);
        if (redraw_csd) {
            render_csd_titlebar(window_surface, destination, visible_size.width());
            window_surface.csd_dirty = false;
        }

        bool const redraw_inset = menubar_h > 0 && (recreate || window_surface.inset_dirty);
        if (redraw_inset) {
            window_surface.draw_inset(destination, { 0, csd_h, visible_size.width(), menubar_h });
            window_surface.inset_dirty = false;
        }

        if (is_fractional && present_fractional(reinterpret_cast<u8 const*>(destination.scanline(0)), destination.pitch(), display_width, display_height))
            return;

        Vector<Gfx::IntRect> surface_damage;
        if (recreate || full_damage) {
            surface_damage.append({ 0, 0, visible_size.width(), logical_height });
        } else {
            if (redraw_csd)
                surface_damage.append({ 0, 0, visible_size.width(), csd_h });
            if (redraw_inset)
                surface_damage.append({ 0, csd_h, visible_size.width(), menubar_h });
            for (auto const& rect : damage) {
                auto clipped = rect.intersected({ 0, 0, visible_size.width(), visible_size.height() });
                if (!clipped.is_empty())
                    surface_damage.append({ clipped.x(), clipped.y() + total_inset, clipped.width(), clipped.height() });
            }
            if (surface_damage.is_empty())
                surface_damage.append({ 0, 0, visible_size.width(), logical_height });
        }

        request_frame();
        bind_bitmap(window_surface.surface, window_surface.composed_buffer, destination, window_surface.buffers, &surface_damage);
        wl_surface_commit(window_surface.surface);
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: presented window {} ({}x{} @{}x incl. {}px csd + {}px inset)", window_surface.window_id, visible_size.width(), visible_size.height(), scale, csd_h, menubar_h);
        return;
    }

    int const display_width = visible_size.width() * scale;
    int const display_height = visible_size.height() * scale;

    update_opaque_region(window_surface, visible_size);

    if (is_fractional || needs_input_region_pixels(window_surface)) {
        size_t const client_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(size.height());
        auto const* source = static_cast<u8 const*>(mmap(nullptr, client_bytes, PROT_READ, MAP_SHARED, client_fd, 0));
        if (source != MAP_FAILED) {
            if (needs_input_region_pixels(window_surface))
                update_input_region(window_surface, source, static_cast<size_t>(pitch), visible_size, scale, 0);
            if (is_fractional) {
                bool const ok = present_fractional(source, static_cast<size_t>(pitch), display_width, display_height);
                munmap(const_cast<u8*>(source), client_bytes);
                if (ok)
                    return;
            } else {
                munmap(const_cast<u8*>(source), client_bytes);
            }
        }
    }

    int fd = dup(client_fd);
    if (fd < 0)
        return;

    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int>(pitch) * size.height());
    if (!pool) {
        close(fd);
        return;
    }

    auto format = has_alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    wl_surface_set_buffer_scale(window_surface.surface, scale);
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, display_width, display_height, pitch, format);
    if (!buffer) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }

    request_frame();
    wl_surface_attach(window_surface.surface, buffer, 0, 0);
    if (full_damage) {
        wl_surface_damage(window_surface.surface, 0, 0, visible_size.width(), visible_size.height());
    } else {
        for (auto const& rect : damage) {
            auto clipped = rect.intersected({ 0, 0, visible_size.width(), visible_size.height() });
            if (!clipped.is_empty())
                wl_surface_damage(window_surface.surface, clipped.x(), clipped.y(), clipped.width(), clipped.height());
        }
    }
    wl_surface_commit(window_surface.surface);
    wl_display_flush(m_display);

    wl_shm_pool_destroy(pool);
    close(fd);

    auto record = make<BufferRecord>();
    record->buffer = buffer;
    wl_buffer_add_listener(buffer, &s_buffer_listener, record.ptr());
    window_surface.buffers.append(move(record));

    dbgln("LibWM/Wayland: presented window {} ({}x{})", window_surface.window_id, visible_size.width(), visible_size.height());
}

void WaylandClient::on_frame_done(wl_callback* callback)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        auto& window_surface = *it.value;
        SurfaceLocker surface_locker(window_surface.mutex);
        if (window_surface.frame_callback != callback)
            continue;
        wl_callback_destroy(callback);
        window_surface.frame_callback = nullptr;
        window_surface.frame_in_flight = false;
        if (window_surface.has_pending) {
            int fd = window_surface.pending_fd;
            auto size = window_surface.pending_size;
            auto visible_size = window_surface.pending_visible_size;
            auto pitch = window_surface.pending_pitch;
            auto has_alpha = window_surface.pending_has_alpha;
            window_surface.pending_fd = -1;
            window_surface.has_pending = false;
            if (fd >= 0) {
                commit_window_content(window_surface, fd, size, visible_size, pitch, has_alpha);
                ::close(fd);
            }
        }
        return;
    }
    wl_callback_destroy(callback);
}

}
