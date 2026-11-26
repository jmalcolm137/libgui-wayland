/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Serenity's global mouse tracking on top of ext-input-observation-v1.
//
// GUI::MouseTracker (LibGUI) asks the window server for the pointer position
// even when it is not over the application's windows; the Eyes and CatDog demos
// are the in-tree users. On Wayland wl_pointer only reports over the surfaces
// with pointer focus, so the shim creates an input position observer per
// toplevel surface. The compositor then reports the pointer relative to that
// surface (inside or outside it), which is turned back into output coordinates
// with the surface origin the desktop reports (serenity_toplevel.position).
//
// The protocol is a draft (wayland-protocols MR !335) implemented by
// SerenityDE and wlmaker, so the XML is vendored in src/protocol/. Positions
// are signed 16:16 fixed point, as the draft specifies.

#include "Environment.h"
#include "WaylandClient.h"
#include "WaylandEvents.h"
#include "WaylandSeat.h"
#include "ext-input-observation-v1-client-protocol.h"

namespace LibWM {

static ext_input_position_observer_v1_listener const s_position_observer_listener = {
    .position = WaylandEvents::input_position,
};

void WaylandEvents::input_position(void* data, ext_input_position_observer_v1*, wl_surface* surface, u32 instance, i32 relative_x, i32 relative_y)
{
    static_cast<WaylandClient*>(data)->on_global_pointer_position(surface, instance, relative_x, relative_y);
}

void WaylandClient::set_global_mouse_tracking(bool enabled)
{
    MutexLocker locker(m_mutex);
    if (m_global_mouse_tracking == enabled)
        return;
    m_global_mouse_tracking = enabled;
    if (!enabled) {
        destroy_mouse_observers();
        m_has_tracked_position = false;
        trace("LibWM/Wayland: global mouse tracking disabled");
        return;
    }

    if (!m_input_observation_manager) {
        // The watcher stays enabled: if the compositor binds the global later,
        // set_input_observation_manager() creates the observers then.
        trace("LibWM/Wayland: no ext-input-observation-v1; global mouse tracking is unavailable");
        return;
    }
    create_mouse_observers();
    trace("LibWM/Wayland: global mouse tracking enabled ({} observer(s))", m_mouse_observers.size());
}

void WaylandClient::create_mouse_observers()
{
    for (auto const& it : m_windows)
        create_mouse_observer(it.key);
}

void WaylandClient::create_mouse_observer(i32 window_id)
{
    MutexLocker locker(m_mutex);
    if (!m_global_mouse_tracking || !m_input_observation_manager)
        return;
    if (m_mouse_observers.contains(window_id))
        return;

    auto* window_surface = find(window_id);
    // Only ordinary toplevels: they are the application's windows, and the
    // desktop reports their origin. Layer surfaces (Taskbar, menus, applets)
    // are positioned by the compositor and do not use MouseTracker.
    if (!window_surface || window_surface->is_layer || !window_surface->surface)
        return;

    auto* pointer = m_seat->wayland_pointer();
    if (!pointer) {
        trace("LibWM/Wayland: no wl_pointer yet; cannot observe the pointer for window {}", window_id);
        return;
    }

    auto* observer = ext_input_observation_manager_v1_create_pointer_observer(m_input_observation_manager, pointer, window_surface->surface);
    if (!observer)
        return;
    ext_input_position_observer_v1_add_listener(observer, &s_position_observer_listener, this);
    m_mouse_observers.set(window_id, observer);
    trace("LibWM/Wayland: pointer observer for window {}", window_id);
}

void WaylandClient::destroy_mouse_observer(i32 window_id)
{
    auto it = m_mouse_observers.find(window_id);
    if (it == m_mouse_observers.end())
        return;
    ext_input_position_observer_v1_destroy(it->value);
    m_mouse_observers.remove(it);
}

void WaylandClient::destroy_mouse_observers()
{
    for (auto const& it : m_mouse_observers)
        ext_input_position_observer_v1_destroy(it.value);
    m_mouse_observers.clear();
}

void WaylandClient::on_global_pointer_position(wl_surface* surface, u32, i32 relative_x, i32 relative_y)
{
    MutexLocker locker(m_mutex);
    if (!m_global_mouse_tracking || !surface)
        return;

    for (auto const& it : m_windows) {
        auto& window_surface = *it.value;
        if (window_surface.surface != surface)
            continue;
        // Layer surfaces are not observed. The compositor's placement
        // (serenity_toplevel.position) is the exact origin; until it arrives,
        // the requested position is used -- the application builds its own
        // screen coordinates from that same value, so relative uses (the Eyes
        // demo) cancel out, and SerenityDE corrects it for exact ones.
        if (window_surface.is_layer || window_surface.logical_size.is_empty())
            return;
        auto const origin = window_surface.origin_known ? window_surface.global_position : window_surface.requested_position;

        auto const position = Gfx::IntPoint {
            origin.x() + static_cast<int>(static_cast<i64>(relative_x) * window_surface.logical_size.width() / 65536),
            origin.y() + static_cast<int>(static_cast<i64>(relative_y) * window_surface.logical_size.height() / 65536),
        };
        // Several observed windows report the same cursor: forward each new
        // position once.
        if (m_has_tracked_position && position == m_last_tracked_position)
            return;
        m_last_tracked_position = position;
        m_has_tracked_position = true;
        if (m_input.track_mouse_move)
            m_input.track_mouse_move(position);
        return;
    }
}

}
