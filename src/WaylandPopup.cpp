/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include "SerenityShell.h"
#include "WaylandOutputs.h"
#include <LibGfx/Bitmap.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace LibWM {

namespace {
using SurfaceLocker = WaylandClient::MutexLocker;
}

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

// --- xdg_popup ----------------------------------------------------------------

static void popup_xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    static_cast<WaylandClient*>(data)->on_surface_configured(surface);
}

static xdg_surface_listener const s_popup_xdg_surface_listener = {
    .configure = popup_xdg_surface_configure,
};

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

void WaylandClient::purge_released_buffers(Popup& popup)
{
    purge_released_buffers(popup.buffers);
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

    bool const direct = m_subcompositor && window_surface->csd_height() == 0
        && SerenityShell::supports_menubar_inset(window_surface->chrome);
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
        m_serenity_shell->set_menubar_inset(window_surface->chrome, inset);
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
        m_serenity_shell->set_menubar_inset(window_surface->chrome, 0);
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

void WaylandClient::present_menubar(WindowSurface& window_surface, Gfx::IntSize visible_size)
{
    if (!window_surface.menubar_surface || !window_surface.draw_inset || window_surface.inset <= 0)
        return;

    int const scale = m_outputs->client_scale();
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
            auto local = anchor.location();
            layer_position = parent_window->layer_output_position.translated(local.x(), local.y());
        } else {
            parent_xdg = parent_window->xdg_surface_object;
            if (!parent_window->menubar_surface && parent_window->csd_height() > 0)
                anchor.translate_by(0, parent_window->csd_height());
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
        if (auto const screen_size = m_outputs->screen_size(); !screen_size.is_empty()) {
            // A button menu flips above its button when it would run off the
            // bottom. A submenu just slides up so it stays on-screen, keeping the
            // item's row reachable (otherwise it would jump above its parent).
            if (!is_submenu && menu_y + size.height() > screen_size.height())
                menu_y -= size.height() + max(0, anchor.height() - 1);
            menu_x = clamp(menu_x, 0, max(0, screen_size.width() - size.width()));
            menu_y = clamp(menu_y, 0, max(0, screen_size.height() - size.height()));
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
    xdg_surface_add_listener(xdg_surface, &s_popup_xdg_surface_listener, this);

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

    size_t const bytes = source.pitch() * source.height();
    // Reuse the popup's shared buffer across hover redraws; only reallocate when
    // the popup's dimensions change.
    if (!p->bitmap || p->bitmap->size() != source.size()) {
        auto buffer = Core::AnonymousBuffer::create_with_size(bytes);
        if (buffer.is_error())
            return;
        p->buffer = buffer.release_value();

        auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, p->buffer, source.size(), 1);
        if (bitmap.is_error())
            return;
        p->bitmap = bitmap.release_value();
    }
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
    if (m_input_state.pointer_popup == popup_id)
        m_input_state.pointer_popup = -1;
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

}
