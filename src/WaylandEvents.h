/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

struct wl_array;
struct wl_callback;
struct wl_registry;
struct wl_surface;
struct wp_fractional_scale_v1;
struct ext_input_position_observer_v1;
struct xdg_activation_token_v1;
struct xdg_popup;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct zxdg_toplevel_decoration_v1;
struct zwlr_layer_surface_v1;

namespace LibWM {

// The C protocol listeners' entry points for the core Wayland objects.
//
// They are a friend of WaylandClient so the client's protocol handlers can stay
// private: the public surface of WaylandClient is the API its callers use, not
// the event plumbing (which is what the C library has to call into).
struct WaylandEvents {
    static void registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version);
    static void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial);
    static void display_link_frame_done(void* data, wl_callback* callback, uint32_t);

    static void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial);
    static void toplevel_configure(void* data, xdg_toplevel* toplevel, i32 width, i32 height, wl_array* states);
    static void toplevel_close(void* data, xdg_toplevel* toplevel);
    static void toplevel_configure_bounds(void*, xdg_toplevel*, i32, i32);
    static void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*);
    static void layer_surface_configure(void* data, zwlr_layer_surface_v1*, u32 serial, u32 width, u32 height);
    static void layer_surface_closed(void* data, zwlr_layer_surface_v1*);
    static void fractional_scale_preferred_scale(void* data, wp_fractional_scale_v1*, u32 scale_v120);
    static void decoration_configure(void* data, zxdg_toplevel_decoration_v1*, u32 mode);
    static void frame_done(void* data, wl_callback* callback, u32);
    static void input_position(void* data, ext_input_position_observer_v1*, wl_surface* surface, u32 instance, i32 relative_x, i32 relative_y);
    static void activation_token_done(void* data, xdg_activation_token_v1*, char const* token);

    static void menu_layer_configure(void* data, zwlr_layer_surface_v1*, u32 serial, u32 width, u32 height);
    static void menu_layer_closed(void* data, zwlr_layer_surface_v1*);
    static void popup_xdg_surface_configure(void* data, xdg_surface* surface, u32 serial);
    static void popup_configure(void*, xdg_popup*, i32 x, i32 y, i32 width, i32 height);
    static void popup_done(void* data, xdg_popup* popup);
    static void popup_repositioned(void*, xdg_popup*, u32);
};

}
