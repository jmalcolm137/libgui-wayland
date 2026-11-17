/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <AK/Assertions.h>
#include <LibCore/Process.h>
#include <LibGfx/Painter.h>
#include <WindowServer/WindowType.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "serenity-window-client-protocol.h"
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

// A menu shown from a layer-surface window is itself an overlay layer surface.
static void menu_layer_configure(void* data, zwlr_layer_surface_v1* layer_surface, uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    static_cast<WaylandClient*>(data)->on_menu_layer_configure(layer_surface, { static_cast<int>(width), static_cast<int>(height) });
}

static void menu_layer_closed(void* data, zwlr_layer_surface_v1* layer_surface)
{
    static_cast<WaylandClient*>(data)->on_menu_layer_closed(layer_surface);
}

static zwlr_layer_surface_v1_listener const s_menu_layer_listener = {
    .configure = menu_layer_configure,
    .closed = menu_layer_closed,
};

// --- serenity_toplevel (compositor-driven window chrome) ----------------------

static void serenity_toplevel_menubar_visibility(void* data, serenity_toplevel* resource, int32_t visible)
{
    static_cast<WaylandClient*>(data)->on_menubar_visibility(resource, visible != 0);
}

static void serenity_toplevel_position(void* data, serenity_toplevel* resource, int32_t x, int32_t y)
{
    static_cast<WaylandClient*>(data)->on_toplevel_position(resource, x, y);
}

static serenity_toplevel_listener const s_serenity_toplevel_listener = {
    .menubar_visibility = serenity_toplevel_menubar_visibility,
    .position = serenity_toplevel_position,
};

// --- zxdg_toplevel_decoration_v1 ----------------------------------------------

static void decoration_configure(void*, zxdg_toplevel_decoration_v1*, uint32_t mode)
{
    dbgln("LibWM/Wayland: server-side decoration mode {}", mode);
}

static zxdg_toplevel_decoration_v1_listener const s_decoration_listener = {
    .configure = decoration_configure,
};

// --- xdg_popup ----------------------------------------------------------------

static void popup_configure(void*, xdg_popup*, int32_t x, int32_t y, int32_t width, int32_t height)
{
    dbgln("LibWM/Wayland: popup configure at {},{} size {}x{}", x, y, width, height);
}
static void popup_done(void* data, xdg_popup* popup)
{
    static_cast<WaylandClient*>(data)->on_popup_done(popup);
}
static void popup_repositioned(void*, xdg_popup*, uint32_t) { }

static xdg_popup_listener const s_popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
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

WaylandClient::Popup* WaylandClient::find_popup(i32 popup_id)
{
    MutexLocker locker(m_mutex);
    auto it = m_popups.find(popup_id);
    return it == m_popups.end() ? nullptr : it->value.ptr();
}

i32 WaylandClient::popup_id_for_surface(wl_surface* surface) const
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_popups) {
        if (it.value->surface == surface)
            return it.value->id;
    }
    return -1;
}

// --- configure & close handlers -----------------------------------------------

void WaylandClient::on_toplevel_configure(xdg_toplevel* toplevel, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized, bool resizing)
{
    MutexLocker locker(m_mutex);
    dbgln("LibWM/Wayland: toplevel configure {}x{} activated={} fullscreen={} maximized={} resizing={}", size.width(), size.height(), activated, fullscreen, maximized, resizing);
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.toplevel != toplevel)
            continue;
        SurfaceLocker surface_locker(w.mutex);
        if (m_input.window_activation)
            m_input.window_activation(w.window_id, activated);

        bool const was_fullscreen = w.fullscreen;
        w.fullscreen = fullscreen || maximized;

        if (!resizing)
            w.resizing = false;

        auto resize_to = [&](Gfx::IntSize content) {
            if (content == w.size)
                return;
            w.size = content;
            if (m_input.window_resize)
                m_input.window_resize(w.window_id, content);
        };

        // Keep the client's last frame and let the compositor scale it to the
        // new size for the duration of an interactive resize. Returns false if
        // there is no viewport to stretch with.
        auto stretch_to = [&](Gfx::IntSize content) {
            if (!w.viewport)
                return false;
            wp_viewport_set_destination(w.viewport, content.width(), content.height() + w.inset);
            w.stretch_active = true;
            wl_surface_commit(w.surface);
            wl_display_flush(m_display);
            return true;
        };

        if (fullscreen || maximized) {
            // Fullscreen/maximize always resizes, even for non-resizable windows.
            if (size.width() > 0 && size.height() > w.inset)
                resize_to({ size.width(), size.height() - w.inset });
        } else if (was_fullscreen && !w.resizable) {
            // Leaving fullscreen: restore the fixed windowed size.
            resize_to(w.fixed_size);
        } else if (w.resizable) {
            // Interactive resize for resizable windows.
            if (size.width() > 0 && size.height() > w.inset) {
                auto content = Gfx::IntSize { size.width(), size.height() - w.inset };
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

void WaylandClient::on_menu_layer_configure(zwlr_layer_surface_v1* layer_surface, Gfx::IntSize)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_popups) {
        auto& popup = *it.value;
        if (popup.layer_surface != layer_surface)
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
}

void WaylandClient::on_menu_layer_closed(zwlr_layer_surface_v1* layer_surface)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_popups) {
        if (it.value->layer_surface == layer_surface) {
            // The compositor vetoed the menu (e.g. it was dismissed); drop it.
            destroy_popup(it.value->id);
            return;
        }
    }
}

void WaylandClient::on_menubar_visibility(serenity_toplevel* resource, bool visible)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        if (it.value->chrome == resource) {
            it.value->menubar_visible = visible;
            if (m_menubar_visibility_changed)
                m_menubar_visibility_changed(it.value->window_id, visible);
            return;
        }
    }
}

void WaylandClient::on_toplevel_position(serenity_toplevel* resource, int32_t x, int32_t y)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_windows) {
        if (it.value->chrome == resource) {
            SurfaceLocker surface_locker(it.value->mutex);
            // Layer surfaces (applets) are laid out by the compositor, so their
            // origin comes from here, not from their anchored corner.
            if (it.value->is_layer)
                it.value->layer_output_position = { x, y };
            else
                it.value->global_position = { x, y };
            if (m_toplevel_position_changed)
                m_toplevel_position_changed(it.value->window_id, Gfx::IntPoint { x, y });
            return;
        }
    }
}

bool WaylandClient::window_menubar_visible(i32 window_id) const
{
    MutexLocker locker(m_mutex);
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return true;
    return it->value->menubar_visible;
}

bool WaylandClient::window_uses_menubar_subsurface(i32 window_id) const
{
    MutexLocker locker(m_mutex);
    auto it = m_windows.find(window_id);
    return it != m_windows.end() && it->value->menubar_surface != nullptr;
}

// --- buffer management & popups -----------------------------------------------

void WaylandClient::purge_released_buffers(Popup& popup)
{
    Vector<NonnullOwnPtr<BufferRecord>> kept;
    for (auto& record : popup.buffers) {
        if (record->released)
            wl_buffer_destroy(record->buffer);
        else
            kept.append(move(record));
    }
    popup.buffers = move(kept);
}

void WaylandClient::purge_released_buffers(WindowSurface& window_surface)
{
    auto purge = [](Vector<NonnullOwnPtr<BufferRecord>>& buffers) {
        Vector<NonnullOwnPtr<BufferRecord>> kept;
        for (auto& record : buffers) {
            if (record->released)
                wl_buffer_destroy(record->buffer);
            else
                kept.append(move(record));
        }
        buffers = move(kept);
    };
    purge(window_surface.buffers);
    purge(window_surface.menubar_buffers);
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

void WaylandClient::set_window_inset(i32 window_id, int inset, Function<void(Gfx::Bitmap&, Gfx::IntRect)> draw)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface)
        return;
    SurfaceLocker surface_locker(window_surface->mutex);
    inset = max(0, inset);
    window_surface->inset = inset;
    window_surface->draw_inset = move(draw);
    // The menubar strip must be redrawn (and re-damaged) on the next present.
    window_surface->inset_dirty = true;
    window_surface->composed_valid = false;

    bool const direct = m_subcompositor && window_surface->chrome
        && wl_proxy_get_version(reinterpret_cast<wl_proxy*>(window_surface->chrome)) >= 2;
    // Drop the old composed buffer if we are switching to the direct path.
    if (direct && window_surface->menubar_surface == nullptr) {
        window_surface->composed_bitmap = nullptr;
        window_surface->composed_buffer = {};
    }

    if (direct && inset > 0) {
        if (!window_surface->menubar_surface) {
            window_surface->menubar_surface = wl_compositor_create_surface(m_compositor);
            window_surface->menubar_subsurface = wl_subcompositor_get_subsurface(
                m_subcompositor, window_surface->menubar_surface, window_surface->surface);
            wl_subsurface_set_desync(window_surface->menubar_subsurface);
        }
        wl_subsurface_set_position(window_surface->menubar_subsurface, 0, -inset);
        serenity_toplevel_set_menubar_inset(window_surface->chrome, inset);
        wl_surface_commit(window_surface->surface);
        wl_display_flush(m_display);
    } else {
        for (auto& record : window_surface->menubar_buffers) {
            if (record->buffer)
                wl_buffer_destroy(record->buffer);
        }
        window_surface->menubar_buffers.clear();
        if (window_surface->menubar_subsurface) {
            wl_subsurface_destroy(window_surface->menubar_subsurface);
            window_surface->menubar_subsurface = nullptr;
        }
        if (window_surface->menubar_surface) {
            wl_surface_destroy(window_surface->menubar_surface);
            window_surface->menubar_surface = nullptr;
        }
        window_surface->menubar_bitmap = nullptr;
        window_surface->menubar_buffer = {};
        if (window_surface->chrome && wl_proxy_get_version(reinterpret_cast<wl_proxy*>(window_surface->chrome)) >= 2) {
            serenity_toplevel_set_menubar_inset(window_surface->chrome, 0);
            wl_display_flush(m_display);
        }
    }

    // Non-resizable windows are pinned to their original size (plus inset);
    // the compositor still overrides this to go fullscreen.
    if (!window_surface->resizable && window_surface->toplevel) {
        int w = window_surface->fixed_size.width();
        int h = window_surface->fixed_size.height() + inset;
        xdg_toplevel_set_min_size(window_surface->toplevel, w, h);
        xdg_toplevel_set_max_size(window_surface->toplevel, w, h);
        wl_display_flush(m_display);
    }
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

void WaylandClient::create_popup(i32 popup_id, i32 parent_window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu)
{
    MutexLocker locker(m_mutex);
    if (!m_compositor || !m_wm_base || size.is_empty())
        return;

    // A layer-surface window (the Desktop, Taskbar, applets) has no xdg parent,
    // so its menus become overlay layer surfaces positioned at the anchor.
    bool layer_parent = false;
    Gfx::IntPoint layer_position;
    xdg_surface* parent_xdg = nullptr;
    if (parent_popup_id != -1) {
        auto* parent_popup = find_popup(parent_popup_id);
        if (!parent_popup)
            return;
        if (parent_popup->is_layer) {
            layer_parent = true;
            // A submenu opens to the right of the parent menu, at the item's row.
            layer_position = parent_popup->layer_position.translated(anchor.right(), anchor.y());
        } else {
            parent_xdg = parent_popup->xdg_surface_object;
        }
    } else {
        auto* parent_window = find(parent_window_id);
        if (!parent_window)
            return;
        if (parent_window->is_layer) {
            layer_parent = true;
            // WindowServerConnection has already made the anchor relative to the parent surface. A layer window's
            // requested_position is its screen origin, so subtracting it here discards the origin and leaves menus
            // at the window-local position (the Taskbar menu used to open near the top of the screen).
            auto local = anchor.location();
            layer_position = parent_window->layer_output_position.translated(local.x(), local.y());
        } else {
            parent_xdg = parent_window->xdg_surface_object;
        }
    }

    if (layer_parent) {
        if (!m_layer_shell)
            return;
        auto* surface = wl_compositor_create_surface(m_compositor);
        auto* layer_surface = zwlr_layer_shell_v1_get_layer_surface(m_layer_shell, surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "serenity-menu");
        zwlr_layer_surface_v1_add_listener(layer_surface, &s_menu_layer_listener, this);
        zwlr_layer_surface_v1_set_size(layer_surface, size.width(), size.height());
        zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        // Keep the menu on-screen: layer surfaces are placed by margin from the
        // anchored corner and are not clamped for us. If it would run off the
        // bottom, flip it above the anchor (as WindowServer does for buttons).
        int menu_x = layer_position.x();
        int menu_y = layer_position.y();
        if (!m_output_state.screen_size.is_empty()) {
            // A button menu flips above its button when it would run off the
            // bottom. A submenu just slides up so it stays on-screen, keeping the
            // item's row reachable (otherwise it would jump above its parent).
            if (!is_submenu && menu_y + size.height() > m_output_state.screen_size.height())
                menu_y -= size.height() + max(0, anchor.height() - 1);
            menu_x = clamp(menu_x, 0, max(0, m_output_state.screen_size.width() - size.width()));
            menu_y = clamp(menu_y, 0, max(0, m_output_state.screen_size.height() - size.height()));
        }
        layer_position = { menu_x, menu_y };
        zwlr_layer_surface_v1_set_margin(layer_surface, layer_position.y(), 0, 0, layer_position.x());
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
        wl_surface_commit(surface);

        auto p = make<Popup>();
        p->id = popup_id;
        p->surface = surface;
        p->layer_surface = layer_surface;
        p->is_layer = true;
        p->layer_position = layer_position;
        m_popups.set(popup_id, move(p));
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: menu layer {} created ({}x{}) at {},{} submenu={}", popup_id, size.width(), size.height(), layer_position.x(), layer_position.y(), is_submenu);
        return;
    }

    if (!parent_xdg)
        return;

    auto* surface = wl_compositor_create_surface(m_compositor);
    auto* xdg_surface = xdg_wm_base_get_xdg_surface(m_wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &s_xdg_surface_listener, this);

    auto* positioner = xdg_wm_base_create_positioner(m_wm_base);
    xdg_positioner_set_size(positioner, size.width(), size.height());
    xdg_positioner_set_anchor_rect(positioner, anchor.x(), anchor.y(), anchor.width(), anchor.height());
    xdg_positioner_set_anchor(positioner, is_submenu ? XDG_POSITIONER_ANCHOR_TOP_RIGHT : XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    auto* popup = xdg_surface_get_popup(xdg_surface, parent_xdg, positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(popup, &s_popup_listener, this);
    // Only the root popup grabs; nested popups inherit the grab.
    if (!is_submenu && m_input_state.seat)
        xdg_popup_grab(popup, m_input_state.seat, m_input_state.last_serial);
    wl_surface_commit(surface);

    auto p = make<Popup>();
    p->id = popup_id;
    p->surface = surface;
    p->xdg_surface_object = xdg_surface;
    p->popup = popup;
    m_popups.set(popup_id, move(p));
    wl_display_flush(m_display);
    dbgln("LibWM/Wayland: popup {} created ({}x{}) at {},{} submenu={}", popup_id, size.width(), size.height(), anchor.x(), anchor.y(), is_submenu);
}

void WaylandClient::present_popup(i32 popup_id, Gfx::Bitmap const& source)
{
    MutexLocker locker(m_mutex);
    auto* p = find_popup(popup_id);
    if (!p || !m_shm || source.size().is_empty())
        return;
    purge_released_buffers(*p);

    size_t bytes = source.pitch() * source.height();
    auto buffer = Core::AnonymousBuffer::create_with_size(bytes);
    if (buffer.is_error())
        return;
    p->buffer = buffer.release_value();

    auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, p->buffer, source.size(), 1);
    if (bitmap.is_error())
        return;
    p->bitmap = bitmap.release_value();
    memcpy(p->bitmap->scanline(0), source.scanline(0), bytes);

    // The compositor requires an xdg_surface to be configured before a buffer is
    // attached; defer until the configure event arrives.
    if (!p->configured) {
        p->pending = p->bitmap;
        return;
    }

    bind_bitmap(p->surface, p->buffer, *p->bitmap, p->buffers);
    wl_surface_commit(p->surface);
    wl_display_flush(m_display);
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

void WaylandClient::destroy_popup(i32 popup_id)
{
    MutexLocker locker(m_mutex);
    auto it = m_popups.find(popup_id);
    if (it == m_popups.end())
        return;
    auto& popup = *it->value;
    for (auto& record : popup.buffers) {
        if (record->buffer)
            wl_buffer_destroy(record->buffer);
    }
    if (popup.layer_surface)
        zwlr_layer_surface_v1_destroy(popup.layer_surface);
    if (popup.popup)
        xdg_popup_destroy(popup.popup);
    if (popup.xdg_surface_object)
        xdg_surface_destroy(popup.xdg_surface_object);
    if (popup.surface)
        wl_surface_destroy(popup.surface);
    m_popups.remove(it);
    wl_display_flush(m_display);
}

void WaylandClient::on_popup_done(xdg_popup* popup)
{
    MutexLocker locker(m_mutex);
    for (auto const& it : m_popups) {
        if (it.value->popup == popup) {
            if (m_input.popup_closed)
                m_input.popup_closed(it.value->id);
            return;
        }
    }
}

// --- window lifecycle & presentation ------------------------------------------

void WaylandClient::create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize size, ByteString const& title, bool has_alpha, bool resizable, i32 window_type, i32 window_mode, i32 parent_window_id, Gfx::IntRect const& launch_origin)
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
            window_surface->layer_output_position = { 0, m_output_state.screen_size.height() - size.height() };
            break;
        case WindowServer::WindowType::Applet:
            window_surface->layer_output_position = { m_output_state.screen_size.width() - size.width(), m_output_state.screen_size.height() - size.height() };
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
        if (m_serenity_window_manager) {
            if (auto* chrome = serenity_window_manager_get_toplevel(m_serenity_window_manager, layer_surface_object)) {
                window_surface->chrome = chrome;
                serenity_toplevel_add_listener(chrome, &s_serenity_toplevel_listener, this);
            }
        }

        m_windows.set(window_id, move(window_surface));
        wl_display_flush(m_display);
        return;
    }

    // Popup windows (ComboBox dropdowns, autocomplete, tooltips, menus opened as
    // windows) are frameless overlays the client positions itself. As xdg
    // toplevels the compositor would decorate and place them, so they would not
    // work as dropdowns. Mirror the server-rendered menu popups: an overlay layer
    // surface anchored top-left at the client's requested position.
    // Notifications use the same mechanism: the NotificationServer positions them
    // (top-right) and they must not be decorated or enter the window list.
    if (m_layer_shell && (type == WindowServer::WindowType::Popup || type == WindowServer::WindowType::Autocomplete || type == WindowServer::WindowType::Tooltip || type == WindowServer::WindowType::Menu || type == WindowServer::WindowType::Notification)) {
        bool const is_notification = type == WindowServer::WindowType::Notification;
        bool const is_tooltip = type == WindowServer::WindowType::Tooltip;
        // Notifications and tooltips take no keyboard focus (they must never
        // steal input); dropdowns need it on demand.
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
    window_surface->fixed_size = size;
    if (m_viewporter)
        window_surface->viewport = wp_viewporter_get_viewport(m_viewporter, surface);

    // Let the compositor drive this window's chrome (the menu bar).
    if (m_serenity_window_manager) {
        if (auto* chrome = serenity_window_manager_get_toplevel(m_serenity_window_manager, surface)) {
            window_surface->chrome = chrome;
            serenity_toplevel_add_listener(chrome, &s_serenity_toplevel_listener, this);
            // Tell the compositor the window's mode (modal chain): a blocking
            // dialog preempts its chain and must keep above its parent.
            serenity_toplevel_set_window_mode(chrome, static_cast<uint32_t>(window_mode));
            // The launcher's origin rect (SerenityOS __libgui_launch_origin_rect,
            // read by GUI::Window), so the compositor can animate the launch.
            if (!launch_origin.is_empty())
                serenity_toplevel_set_launch_origin(chrome, launch_origin.x(), launch_origin.y(), launch_origin.width(), launch_origin.height());
        }
    }

    // A non-resizable window pins min == max (fullscreen still overrides this).
    if (!resizable) {
        xdg_toplevel_set_min_size(toplevel, size.width(), size.height());
        xdg_toplevel_set_max_size(toplevel, size.width(), size.height());
    }

    // Ask the compositor to draw server-side decorations (titlebar, buttons,
    // and therefore a draggable frame). Without this, compositors assume the
    // client draws its own frame and add no chrome.
    if (m_decoration_manager) {
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
        if (window_surface.chrome)
            serenity_toplevel_destroy(window_surface.chrome);
        // The decoration is a child of the toplevel and must be destroyed first, or
        // the compositor raises a "destroyed before" protocol error.
        if (window_surface.decoration)
            zxdg_toplevel_decoration_v1_destroy(window_surface.decoration);
        if (window_surface.toplevel)
            xdg_toplevel_destroy(window_surface.toplevel);
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

    // Layer surfaces are positioned by the compositor; only the size is ours to
    // request. Panels stretch across their anchored edges, so only the height
    // (the panel thickness) is meaningful there.
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
        if (!window_surface->toplevel)
            return;
        if (maximized)
            xdg_toplevel_set_maximized(window_surface->toplevel);
        else
            xdg_toplevel_unset_maximized(window_surface->toplevel);
        wl_display_flush(m_display);
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
    if (auto* window_surface = find(window_id)) {
        if (!window_surface->chrome)
            return;
        // Older compositors bind the chrome at version 2 and have no request for it.
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(window_surface->chrome)) < 3)
            return;
        serenity_toplevel_set_always_on_top(window_surface->chrome, always_on_top ? 1 : 0);
        wl_display_flush(m_display);
    }
}

void WaylandClient::set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const& icon)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!window_surface || !window_surface->chrome)
        return;

    if (!icon.is_valid() || !icon.bitmap()) {
        serenity_toplevel_set_icon(window_surface->chrome, 0, 0, 0, 0, -1);
        wl_display_flush(m_display);
        return;
    }

    auto const& bitmap = *icon.bitmap();
    int const width = bitmap.width();
    int const height = bitmap.height();
    int const stride = bitmap.pitch();
    if (width <= 0 || height <= 0 || stride <= 0)
        return;

    size_t const size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    int fd = memfd_create("sde-window-icon", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return;
    }
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping != MAP_FAILED) {
        for (int y = 0; y < height; ++y)
            memcpy(static_cast<u8*>(mapping) + static_cast<size_t>(y) * stride, bitmap.scanline(y), static_cast<size_t>(stride));
        munmap(mapping, size);
    }
    // wl_shm ARGB8888 is little-endian B,G,R,A, matching Gfx::BitmapFormat::BGRA8888.
    serenity_toplevel_set_icon(window_surface->chrome, width, height, stride, 0, fd);
    close(fd);
    wl_display_flush(m_display);
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
    // the newest content and attach it when the frame callback lands. A resize
    // that produces several configures/paints within one refresh interval thus
    // collapses to a single present.
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

void WaylandClient::present_menubar(WindowSurface& window_surface, Gfx::IntSize visible_size)
{
    if (!window_surface.menubar_surface || !window_surface.draw_inset || window_surface.inset <= 0)
        return;

    int const scale = client_scale();
    Gfx::IntSize const logical { visible_size.width(), window_surface.inset };
    bool const recreate = !window_surface.menubar_bitmap
        || window_surface.menubar_bitmap->size() != logical
        || window_surface.menubar_bitmap->scale() != scale;
    if (recreate) {
        size_t const pitch = Gfx::Bitmap::minimum_pitch(logical.width() * scale, Gfx::BitmapFormat::BGRA8888);
        size_t const bytes = pitch * static_cast<size_t>(logical.height() * scale);
        auto buffer = Core::AnonymousBuffer::create_with_size(bytes);
        if (buffer.is_error())
            return;
        window_surface.menubar_buffer = buffer.release_value();
        auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(
            Gfx::BitmapFormat::BGRA8888, window_surface.menubar_buffer, logical, scale);
        if (bitmap.is_error())
            return;
        window_surface.menubar_bitmap = bitmap.release_value();
        window_surface.inset_dirty = true;
    }

    if (window_surface.inset_dirty) {
        window_surface.draw_inset(*window_surface.menubar_bitmap, { 0, 0, logical.width(), logical.height() });
        window_surface.inset_dirty = false;
    }

    bind_bitmap(window_surface.menubar_surface, window_surface.menubar_buffer, *window_surface.menubar_bitmap,
        window_surface.menubar_buffers);
    wl_surface_commit(window_surface.menubar_surface);
}

void WaylandClient::commit_window_content(WindowSurface& window_surface, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha)
{
    MutexLocker locker(m_mutex);
    // A display link enabled before the first present starts ticking once there is a
    // surface to pace it from.
    if (m_display_link.enabled)
        request_display_link_frame();
    auto request_frame = [&] {
        window_surface.frame_callback = wl_surface_frame(window_surface.surface);
        wl_callback_add_listener(window_surface.frame_callback, &s_frame_listener, this);
        window_surface.frame_in_flight = true;
    };

    // After an interactive resize the client renders the final size; drop the
    // stretch so the surface shows the buffer at its native size. Doing it here
    // (rather than at the end of the drag) avoids a snap back to the old size.
    if (window_surface.stretch_active && !window_surface.resizing) {
        if (window_surface.viewport)
            wp_viewport_set_destination(window_surface.viewport, -1, -1);
        window_surface.stretch_active = false;
    }

    purge_released_buffers(window_surface);

    // Damage accumulated since the last present (logical, visible coordinates).
    // Empty (or an overflow) means "the whole window".
    Vector<Gfx::IntRect> damage = move(window_surface.damage);
    window_surface.damage.clear();
    bool const full_damage = window_surface.damage_full || damage.is_empty();
    window_surface.damage_full = false;

    // Direct path: the menubar is a small subsurface above the client content.
    // The compositor offsets the content tree by the same inset (see
    // serenity_toplevel.set_menubar_inset). Only fall back to the old
    // full-window composition if the compositor cannot do that.
    if (window_surface.menubar_surface) {
        present_menubar(window_surface, visible_size);
        // The menubar is a subsurface above the xdg surface. Keep the xdg window
        // geometry on the client content so the compositor does not treat the
        // negative subsurface position as the window origin (which would shift
        // the whole frame down by the inset).
        if (window_surface.xdg_surface_object)
            xdg_surface_set_window_geometry(window_surface.xdg_surface_object, 0, 0,
                visible_size.width(), visible_size.height());
    }

    if (window_surface.inset > 0 && window_surface.draw_inset && !window_surface.menubar_surface) {
        // Compose at the client's backing-store scale: the menubar and the window's
        // visible region are both scaled, and the composed buffer is presented with
        // buffer_scale.
        int const scale = client_scale();
        int const logical_height = visible_size.height() + window_surface.inset;
        int const display_width = visible_size.width() * scale;
        int const display_height = logical_height * scale;
        size_t const composed_pitch = Gfx::Bitmap::minimum_pitch(display_width, Gfx::BitmapFormat::BGRA8888);
        size_t const composed_height = static_cast<size_t>(display_height);

        // Reuse the composed buffer across presents; recreate only on a size or
        // scale change (this used to allocate + fully memcpy on every commit).
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
        // Copy only the damaged content rows (everything on recreate/full damage).
        auto copy_rect = [&](Gfx::IntRect rect) {
            rect = rect.intersected({ 0, 0, visible_size.width(), visible_size.height() });
            if (rect.is_empty())
                return;
            size_t const row_bytes = static_cast<size_t>(rect.width()) * scale * 4;
            for (int row = rect.y() * scale; row < (rect.y() + rect.height()) * scale; ++row)
                memcpy(destination.scanline(window_surface.inset * scale + row) + static_cast<size_t>(rect.x()) * scale,
                    source + static_cast<size_t>(row) * pitch + static_cast<size_t>(rect.x()) * scale * 4,
                    row_bytes);
        };
        if (recreate || full_damage) {
            copy_rect({ 0, 0, visible_size.width(), visible_size.height() });
        } else {
            for (auto const& rect : damage)
                copy_rect(rect);
        }
        munmap(const_cast<u8*>(source), client_bytes);

        // Redraw the menubar strip only when it can have changed.
        bool const redraw_inset = recreate || window_surface.inset_dirty;
        if (redraw_inset) {
            window_surface.draw_inset(destination, { 0, 0, visible_size.width(), window_surface.inset });
            window_surface.inset_dirty = false;
        }

        // Damage in surface coordinates: the content is shifted down by the inset.
        Vector<Gfx::IntRect> surface_damage;
        if (recreate || full_damage) {
            surface_damage.append({ 0, 0, visible_size.width(), logical_height });
        } else {
            if (redraw_inset)
                surface_damage.append({ 0, 0, visible_size.width(), window_surface.inset });
            for (auto const& rect : damage) {
                auto clipped = rect.intersected({ 0, 0, visible_size.width(), visible_size.height() });
                if (!clipped.is_empty())
                    surface_damage.append({ clipped.x(), clipped.y() + window_surface.inset, clipped.width(), clipped.height() });
            }
            if (surface_damage.is_empty())
                surface_damage.append({ 0, 0, visible_size.width(), logical_height });
        }

        request_frame();
        bind_bitmap(window_surface.surface, window_surface.composed_buffer, destination, window_surface.buffers, &surface_damage);
        wl_surface_commit(window_surface.surface);
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: presented window {} ({}x{} @{}x incl. {}px inset)", window_surface.window_id, visible_size.width(), visible_size.height(), scale, window_surface.inset);
        return;
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
    // Present the window's *visible* region at the client's backing-store scale.
    // The backing store may be larger than the window during a resize, so the
    // scale must come from the client's actual scale, not size/visible (which
    // would yield a bogus ratio like 4x mid-resize).
    int const scale = client_scale();
    int const display_width = visible_size.width() * scale;
    int const display_height = visible_size.height() * scale;
    wl_surface_set_buffer_scale(window_surface.surface, scale);
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, display_width, display_height, pitch, format);
    if (!buffer) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }

    request_frame();
    wl_surface_attach(window_surface.surface, buffer, 0, 0);
    // The backing store is the client's shared memory, so only the changed
    // regions need re-reading; the rest is unchanged from the previous present.
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

    // The pool's fd has been transferred by the flush above; buffers outlive it.
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
