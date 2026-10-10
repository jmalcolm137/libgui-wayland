/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <Kernel/API/KeyCode.h>
#include <LibCore/Notifier.h>
#include <LibGUI/Event.h>
#include <LibGfx/Painter.h>
#include <WindowServer/WindowType.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "serenity-window-client-protocol.h"
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
        self.set_serenity_window_manager(reinterpret_cast<serenity_window_manager*>(wl_registry_bind(registry, name, &serenity_window_manager_interface, 1)));
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

// --- xdg_surface_object --------------------------------------------------------------

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

static serenity_toplevel_listener const s_serenity_toplevel_listener = {
    .menubar_visibility = serenity_toplevel_menubar_visibility,
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

// --- wl_seat / wl_pointer / wl_keyboard --------------------------------------

static KeyCode serenity_key_code_from_evdev(u32 key)
{
    switch (key) {
    case KEY_ESC:
        return Key_Escape;
    case KEY_TAB:
        return Key_Tab;
    case KEY_BACKSPACE:
        return Key_Backspace;
    case KEY_ENTER:
        return Key_Return;
    case KEY_INSERT:
        return Key_Insert;
    case KEY_DELETE:
        return Key_Delete;
    case KEY_HOME:
        return Key_Home;
    case KEY_END:
        return Key_End;
    case KEY_LEFT:
        return Key_Left;
    case KEY_UP:
        return Key_Up;
    case KEY_RIGHT:
        return Key_Right;
    case KEY_DOWN:
        return Key_Down;
    case KEY_PAGEUP:
        return Key_PageUp;
    case KEY_PAGEDOWN:
        return Key_PageDown;
    case KEY_LEFTSHIFT:
        return Key_LeftShift;
    case KEY_RIGHTSHIFT:
        return Key_RightShift;
    case KEY_LEFTCTRL:
        return Key_LeftControl;
    case KEY_RIGHTCTRL:
        return Key_RightControl;
    case KEY_LEFTALT:
        return Key_LeftAlt;
    case KEY_RIGHTALT:
        return Key_RightAlt;
    case KEY_LEFTMETA:
        return Key_LeftSuper;
    case KEY_RIGHTMETA:
        return Key_RightSuper;
    case KEY_CAPSLOCK:
        return Key_CapsLock;
    case KEY_NUMLOCK:
        return Key_NumLock;
    case KEY_SCROLLLOCK:
        return Key_ScrollLock;
    case KEY_SPACE:
        return Key_Space;
    case KEY_MINUS:
        return Key_Minus;
    case KEY_EQUAL:
        return Key_Equal;
    case KEY_LEFTBRACE:
        return Key_LeftBracket;
    case KEY_RIGHTBRACE:
        return Key_RightBracket;
    case KEY_BACKSLASH:
        return Key_Backslash;
    case KEY_SEMICOLON:
        return Key_Semicolon;
    case KEY_APOSTROPHE:
        return Key_Apostrophe;
    case KEY_GRAVE:
        return Key_Backtick;
    case KEY_COMMA:
        return Key_Comma;
    case KEY_DOT:
        return Key_Period;
    case KEY_SLASH:
        return Key_Slash;
    case KEY_1:
        return Key_1;
    case KEY_2:
        return Key_2;
    case KEY_3:
        return Key_3;
    case KEY_4:
        return Key_4;
    case KEY_5:
        return Key_5;
    case KEY_6:
        return Key_6;
    case KEY_7:
        return Key_7;
    case KEY_8:
        return Key_8;
    case KEY_9:
        return Key_9;
    case KEY_0:
        return Key_0;
    case KEY_F1:
        return Key_F1;
    case KEY_F2:
        return Key_F2;
    case KEY_F3:
        return Key_F3;
    case KEY_F4:
        return Key_F4;
    case KEY_F5:
        return Key_F5;
    case KEY_F6:
        return Key_F6;
    case KEY_F7:
        return Key_F7;
    case KEY_F8:
        return Key_F8;
    case KEY_F9:
        return Key_F9;
    case KEY_F10:
        return Key_F10;
    case KEY_F11:
        return Key_F11;
    case KEY_F12:
        return Key_F12;
    case KEY_A:
        return Key_A;
    case KEY_B:
        return Key_B;
    case KEY_C:
        return Key_C;
    case KEY_D:
        return Key_D;
    case KEY_E:
        return Key_E;
    case KEY_F:
        return Key_F;
    case KEY_G:
        return Key_G;
    case KEY_H:
        return Key_H;
    case KEY_I:
        return Key_I;
    case KEY_J:
        return Key_J;
    case KEY_K:
        return Key_K;
    case KEY_L:
        return Key_L;
    case KEY_M:
        return Key_M;
    case KEY_N:
        return Key_N;
    case KEY_O:
        return Key_O;
    case KEY_P:
        return Key_P;
    case KEY_Q:
        return Key_Q;
    case KEY_R:
        return Key_R;
    case KEY_S:
        return Key_S;
    case KEY_T:
        return Key_T;
    case KEY_U:
        return Key_U;
    case KEY_V:
        return Key_V;
    case KEY_W:
        return Key_W;
    case KEY_X:
        return Key_X;
    case KEY_Y:
        return Key_Y;
    case KEY_Z:
        return Key_Z;
    default:
        return Key_Invalid;
    }
}

static void seat_capabilities(void* data, wl_seat*, uint32_t capabilities)
{
    static_cast<WaylandClient*>(data)->on_seat_capabilities(capabilities);
}

static void seat_name(void*, wl_seat*, char const*) { }

static wl_seat_listener const s_seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void pointer_enter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandClient*>(data)->note_input_serial(serial);
    static_cast<WaylandClient*>(data)->on_pointer_enter(surface, { wl_fixed_to_int(sx), wl_fixed_to_int(sy) });
}

static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*)
{
    static_cast<WaylandClient*>(data)->on_pointer_leave();
}

static void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandClient*>(data)->on_pointer_motion({ wl_fixed_to_int(sx), wl_fixed_to_int(sy) });
}

static void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
    static_cast<WaylandClient*>(data)->note_input_serial(serial);
    static_cast<WaylandClient*>(data)->on_pointer_button(button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value)
{
    int delta = wl_fixed_to_int(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        static_cast<WaylandClient*>(data)->on_pointer_axis(0, delta);
    else
        static_cast<WaylandClient*>(data)->on_pointer_axis(delta, 0);
}

static void pointer_frame(void*, wl_pointer*) { }
static void pointer_axis_source(void*, wl_pointer*, uint32_t) { }
static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) { }
static void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) { }
static void pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) { }
static void pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) { }
static void pointer_warp(void*, wl_pointer*, wl_fixed_t, wl_fixed_t) { }

static wl_pointer_listener const s_pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
    .warp = pointer_warp,
};

static void keyboard_keymap(void* data, wl_keyboard*, uint32_t, int32_t fd, uint32_t size)
{
    static_cast<WaylandClient*>(data)->on_keyboard_keymap(fd, size);
}

static void keyboard_enter(void* data, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array*)
{
    static_cast<WaylandClient*>(data)->note_input_serial(serial);
    static_cast<WaylandClient*>(data)->on_keyboard_focus(surface, true);
}

static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*)
{
    static_cast<WaylandClient*>(data)->on_keyboard_focus(nullptr, false);
}

static void keyboard_key(void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
{
    static_cast<WaylandClient*>(data)->note_input_serial(serial);
    static_cast<WaylandClient*>(data)->on_keyboard_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    static_cast<WaylandClient*>(data)->on_keyboard_modifiers(depressed, latched, locked, group, serial);
}

static void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) { }

static wl_keyboard_listener const s_keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// --- wl_data_device / wl_data_offer / wl_data_source -------------------------

static void data_device_data_offer(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandClient*>(data)->on_data_offer(offer);
}

static void data_device_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) { }
static void data_device_leave(void*, wl_data_device*) { }
static void data_device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) { }
static void data_device_drop(void*, wl_data_device*) { }

static void data_device_selection(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandClient*>(data)->on_selection(offer);
}

static wl_data_device_listener const s_data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void data_offer_offer(void* data, wl_data_offer* offer, char const* mime_type)
{
    static_cast<WaylandClient*>(data)->on_data_offer_mime(offer, mime_type);
}

static void data_offer_source_actions(void*, wl_data_offer*, uint32_t) { }
static void data_offer_action(void*, wl_data_offer*, uint32_t) { }

static wl_data_offer_listener const s_data_offer_listener = {
    .offer = data_offer_offer,
    .source_actions = data_offer_source_actions,
    .action = data_offer_action,
};

static void data_source_target(void*, wl_data_source*, char const*) { }

static void data_source_send(void* data, wl_data_source* source, char const* mime_type, int32_t fd)
{
    static_cast<WaylandClient*>(data)->on_source_send(source, mime_type, fd);
}

static void data_source_cancelled(void* data, wl_data_source* source)
{
    static_cast<WaylandClient*>(data)->on_source_cancelled(source);
}

static void data_source_dnd_drop_performed(void*, wl_data_source*) { }
static void data_source_dnd_finished(void*, wl_data_source*) { }
static void data_source_action(void*, wl_data_source*, uint32_t) { }

static wl_data_source_listener const s_data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_dnd_drop_performed,
    .dnd_finished = data_source_dnd_finished,
    .action = data_source_action,
};

// --- WaylandClient ------------------------------------------------------------

WaylandClient& WaylandClient::the()
{
    static WaylandClient s_client;
    return s_client;
}

ErrorOr<void> WaylandClient::ensure_connected()
{
    if (m_display)
        return { };

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
        m_screen_size.width(), m_screen_size.height(), m_scale, m_scale);
    return { };
}

void WaylandClient::dispatch()
{
    if (!m_display)
        return;
    wl_display_dispatch(m_display);
    // Requests queued while handling events (notably xdg_wm_base.pong) must be
    // flushed now; waiting for the next event would make the compositor think we
    // are unresponsive.
    wl_display_flush(m_display);
}

void WaylandClient::add_output(wl_output* output)
{
    m_output = output;
    m_outputs.append(output);
    wl_output_add_listener(output, &s_output_listener, this);
    if (m_xdg_output_manager) {
        auto* xdg_output = zxdg_output_manager_v1_get_xdg_output(m_xdg_output_manager, output);
        zxdg_output_v1_add_listener(xdg_output, &s_xdg_output_listener, this);
    }
}

void WaylandClient::set_xdg_output_manager(zxdg_output_manager_v1* manager)
{
    m_xdg_output_manager = manager;
    for (auto* output : m_outputs) {
        auto* xdg_output = zxdg_output_manager_v1_get_xdg_output(manager, output);
        zxdg_output_v1_add_listener(xdg_output, &s_xdg_output_listener, this);
    }
}

void WaylandClient::add_seat(wl_seat* seat)
{
    m_seat = seat;
    wl_seat_add_listener(seat, &s_seat_listener, this);
    maybe_create_data_device();
}

void WaylandClient::set_data_device_manager(wl_data_device_manager* manager)
{
    m_data_device_manager = manager;
    maybe_create_data_device();
}

void WaylandClient::maybe_create_data_device()
{
    if (m_data_device || !m_seat || !m_data_device_manager)
        return;
    m_data_device = wl_data_device_manager_get_data_device(m_data_device_manager, m_seat);
    dbgln("LibWM/Wayland: data device created: {}", m_data_device != nullptr);
    if (m_data_device)
        wl_data_device_add_listener(m_data_device, &s_data_device_listener, this);
}

void WaylandClient::add_wm_base(xdg_wm_base* wm_base)
{
    m_wm_base = wm_base;
    xdg_wm_base_add_listener(wm_base, &s_wm_base_listener, this);
}

void WaylandClient::on_output_logical_size(wl_output*, Gfx::IntSize logical_size)
{
    if (logical_size.is_empty())
        return;
    m_have_logical_size = true;
    m_screen_size = logical_size;
}

void WaylandClient::on_output_done()
{
    // zxdg_output_v1 gives the real logical size under fractional scaling and
    // takes precedence over mode/scale.
    if (m_have_logical_size)
        return;
    if (m_physical_size.is_empty())
        return;
    m_screen_size = { m_physical_size.width() / m_scale, m_physical_size.height() / m_scale };
}

void WaylandClient::on_seat_capabilities(u32 capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !m_pointer) {
        m_pointer = wl_seat_get_pointer(m_seat);
        wl_pointer_add_listener(m_pointer, &s_pointer_listener, this);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !m_keyboard) {
        m_keyboard = wl_seat_get_keyboard(m_seat);
        wl_keyboard_add_listener(m_keyboard, &s_keyboard_listener, this);
    }
    dbgln("LibWM/Wayland: seat capabilities 0x{:x} (pointer={}, keyboard={})", capabilities,
        m_pointer != nullptr, m_keyboard != nullptr);
}

i32 WaylandClient::window_id_for_surface(wl_surface* surface) const
{
    for (auto const& it : m_windows) {
        if (it.value->surface == surface)
            return it.value->window_id;
    }
    return -1;
}

void WaylandClient::on_pointer_enter(wl_surface* surface, Gfx::IntPoint position)
{
    m_pointer_popup = popup_id_for_surface(surface);
    m_pointer_window = m_pointer_popup >= 0 ? -1 : window_id_for_surface(surface);
    m_pointer_position = position;
    dbgln("LibWM/Wayland: pointer entered {} {} at {},{}",
        m_pointer_popup >= 0 ? "popup" : "window",
        m_pointer_popup >= 0 ? m_pointer_popup : m_pointer_window,
        position.x(), position.y());

    if (m_pointer_popup >= 0) {
        if (m_input.popup_motion)
            m_input.popup_motion(m_pointer_popup, position);
        return;
    }
    if (m_pointer_window >= 0 && m_input.window_entered)
        m_input.window_entered(m_pointer_window);
}

void WaylandClient::on_pointer_leave()
{
    if (m_pointer_popup >= 0) {
        m_pointer_popup = -1;
        m_pointer_window = -1;
        return;
    }
    if (m_pointer_window >= 0 && m_input.menubar_left)
        m_input.menubar_left(m_pointer_window);
    if (m_pointer_window >= 0 && m_input.window_left)
        m_input.window_left(m_pointer_window);
    m_pointer_window = -1;
}

void WaylandClient::on_pointer_motion(Gfx::IntPoint position)
{
    m_pointer_position = position;

    if (m_pointer_popup >= 0) {
        if (m_input.popup_motion)
            m_input.popup_motion(m_pointer_popup, position);
        return;
    }
    if (m_pointer_window < 0)
        return;

    auto* window_surface = find(m_pointer_window);
    int inset = window_surface ? window_surface->inset : 0;
    if (inset > 0 && position.y() < inset) {
        if (m_input.menubar_motion)
            m_input.menubar_motion(m_pointer_window, position);
        return;
    }
    if (m_input.menubar_left)
        m_input.menubar_left(m_pointer_window);
    if (m_input.mouse_move)
        m_input.mouse_move(m_pointer_window, position.translated(0, -inset), m_pointer_buttons, current_modifiers());
}

void WaylandClient::on_pointer_button(u32 button, bool pressed)
{
    u32 serenity_button = 0;
    switch (button) {
    case BTN_LEFT:
        serenity_button = static_cast<u32>(GUI::MouseButton::Primary);
        break;
    case BTN_RIGHT:
        serenity_button = static_cast<u32>(GUI::MouseButton::Secondary);
        break;
    case BTN_MIDDLE:
        serenity_button = static_cast<u32>(GUI::MouseButton::Middle);
        break;
    case BTN_SIDE:
        serenity_button = static_cast<u32>(GUI::MouseButton::Backward);
        break;
    case BTN_EXTRA:
        serenity_button = static_cast<u32>(GUI::MouseButton::Forward);
        break;
    default:
        return;
    }

    if (m_pointer_popup >= 0) {
        if (m_input.popup_button)
            m_input.popup_button(m_pointer_popup, m_pointer_position, pressed);
        return;
    }
    if (m_pointer_window < 0)
        return;

    auto* window_surface = find(m_pointer_window);
    int inset = window_surface ? window_surface->inset : 0;
    if (inset > 0 && m_pointer_position.y() < inset) {
        if (pressed && m_input.menubar_press)
            m_input.menubar_press(m_pointer_window, m_pointer_position);
        return;
    }

    auto position = m_pointer_position.translated(0, -inset);
    if (pressed) {
        m_pointer_buttons |= serenity_button;
        dbgln("LibWM/Wayland: mouse down window {} at {},{} button={}", m_pointer_window, position.x(), position.y(), serenity_button);
        if (m_input.mouse_down)
            m_input.mouse_down(m_pointer_window, position, serenity_button, m_pointer_buttons, current_modifiers());
    } else {
        m_pointer_buttons &= ~serenity_button;
        dbgln("LibWM/Wayland: mouse up window {} at {},{} button={}", m_pointer_window, position.x(), position.y(), serenity_button);
        if (m_input.mouse_up)
            m_input.mouse_up(m_pointer_window, position, serenity_button, m_pointer_buttons, current_modifiers());
    }
}

void WaylandClient::on_pointer_axis(i32 x, i32 y)
{
    if (m_pointer_popup >= 0 || m_pointer_window < 0)
        return;
    auto* window_surface = find(m_pointer_window);
    int inset = window_surface ? window_surface->inset : 0;
    if (m_input.mouse_wheel)
        m_input.mouse_wheel(m_pointer_window, m_pointer_position.translated(0, -inset), m_pointer_buttons, current_modifiers(), x, y);
}

void WaylandClient::on_keyboard_keymap(int fd, u32 size)
{
    if (!m_xkb_context)
        m_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    auto* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    close(fd);

    if (m_xkb_keymap)
        xkb_keymap_unref(m_xkb_keymap);
    m_xkb_keymap = xkb_keymap_new_from_string(m_xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);

    if (m_xkb_state)
        xkb_state_unref(m_xkb_state);
    m_xkb_state = m_xkb_keymap ? xkb_state_new(m_xkb_keymap) : nullptr;

    dbgln("LibWM/Wayland: received keymap ({} bytes), state={}", size, m_xkb_state != nullptr);
}

u32 WaylandClient::current_modifiers() const
{
    return m_modifiers;
}

void WaylandClient::on_keyboard_modifiers(u32 depressed, u32, u32 locked, u32 group, u32)
{
    // We derive modifiers from the key events themselves (see on_keyboard_key),
    // because some compositors don't deliver modifier events for injected or
    // synthetic keys. Keep the xkb layout group in sync.
    dbgln("LibWM/Wayland: modifiers depressed={:#x} locked={:#x} group={}", depressed, locked, group);
}

static u32 modifier_bit_for_evdev(u32 key)
{
    switch (key) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return Mod_Shift;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return Mod_Ctrl;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        return Mod_Alt;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return Mod_Super;
    default:
        return 0;
    }
}

void WaylandClient::on_keyboard_key(u32 key, bool pressed)
{
    if (!m_xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    // Update the xkb state from the key itself so code_point reflects
    // Shift/CapsLock/etc. even without modifier events.
    xkb_state_update_key(m_xkb_state, code, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    u32 code_point = xkb_state_key_get_utf32(m_xkb_state, code);
    u32 key_code = static_cast<u32>(serenity_key_code_from_evdev(key));

    if (u32 bit = modifier_bit_for_evdev(key); bit != 0) {
        if (pressed)
            m_modifiers |= bit;
        else
            m_modifiers &= ~bit;
    }

    if (m_input.key)
        m_input.key(m_focused_window, code_point, key_code, 0, current_modifiers(), key, pressed);
}

void WaylandClient::on_keyboard_focus(wl_surface* surface, bool entered)
{
    m_focused_window = entered ? window_id_for_surface(surface) : -1;
}

void WaylandClient::on_toplevel_configure(xdg_toplevel* toplevel, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized, bool resizing)
{
    dbgln("LibWM/Wayland: toplevel configure {}x{} activated={} fullscreen={} maximized={} resizing={}", size.width(), size.height(), activated, fullscreen, maximized, resizing);
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.toplevel != toplevel)
            continue;
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
    for (auto const& it : m_windows) {
        auto& w = *it.value;
        if (w.layer_surface != layer_surface)
            continue;
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
    for (auto const& it : m_windows) {
        if (it.value->chrome == resource) {
            it.value->menubar_visible = visible;
            if (m_menubar_visibility_changed)
                m_menubar_visibility_changed(it.value->window_id, visible);
            return;
        }
    }
}

bool WaylandClient::window_menubar_visible(i32 window_id) const
{
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return true;
    return it->value->menubar_visible;
}

ByteString WaylandClient::preferred_mime_for(wl_data_offer* offer) const
{
    auto it = m_offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it == m_offer_mime_types.end() || it->value.is_empty())
        return { };
    auto const& mimes = it->value;
    for (auto const& candidate : { "text/plain;charset=utf-8"sv, "text/plain"sv, "UTF8_STRING"sv, "STRING"sv, "text/uri-list"sv, "image/png"sv }) {
        for (auto const& mime : mimes) {
            if (mime == candidate)
                return mime;
        }
    }
    // No known type: fall back to the source's first offer (e.g. image/png).
    return mimes.first();
}

void WaylandClient::on_data_offer(wl_data_offer* offer)
{
    dbgln("LibWM/Wayland: data offer {}", static_cast<void*>(offer));
    m_pending_offers.append(offer);
    m_offer_mime_types.set(reinterpret_cast<u64>(offer), { });
    wl_data_offer_add_listener(offer, &s_data_offer_listener, this);
}

void WaylandClient::on_data_offer_mime(wl_data_offer* offer, char const* mime_type)
{
    auto it = m_offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it != m_offer_mime_types.end())
        it->value.append(ByteString(mime_type));
}

void WaylandClient::on_selection(wl_data_offer* offer)
{
    for (auto* pending : m_pending_offers) {
        if (pending != offer) {
            m_offer_mime_types.remove(reinterpret_cast<u64>(pending));
            wl_data_offer_destroy(pending);
        }
    }
    m_pending_offers.clear();
    if (offer)
        m_pending_offers.append(offer);
    m_current_offer = offer;

    ByteString mime;
    if (offer) {
        mime = preferred_mime_for(offer);
        // Serenity clients key on "text/plain"; normalize the charset variant.
        if (mime.starts_with("text/plain"sv))
            mime = ByteString("text/plain");
    }
    dbgln("LibWM/Wayland: selection changed (mime='{}')", mime);
    if (m_clipboard_changed)
        m_clipboard_changed(mime);
}

void WaylandClient::on_source_send(wl_data_source*, char const* mime_type, int fd)
{
    auto it = m_clipboard_offers.find(ByteString(mime_type));
    if (it != m_clipboard_offers.end()) {
        auto const& data = it->value;
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = write(fd, data.data() + written, data.size() - written);
            if (n <= 0)
                break;
            written += static_cast<size_t>(n);
        }
    }
    close(fd);
}

void WaylandClient::on_source_cancelled(wl_data_source* source)
{
    if (source == m_data_source)
        m_data_source = nullptr;
    if (source)
        wl_data_source_destroy(source);
}

WaylandClient::Popup* WaylandClient::find_popup(i32 popup_id)
{
    auto it = m_popups.find(popup_id);
    return it == m_popups.end() ? nullptr : it->value.ptr();
}

i32 WaylandClient::popup_id_for_surface(wl_surface* surface) const
{
    for (auto const& it : m_popups) {
        if (it.value->surface == surface)
            return it.value->id;
    }
    return -1;
}

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

void WaylandClient::bind_bitmap(wl_surface* surface, Core::AnonymousBuffer const& buffer, Gfx::Bitmap const& bitmap, Vector<NonnullOwnPtr<BufferRecord>>& buffers)
{
    if (!m_shm)
        return;
    int fd = dup(buffer.fd());
    if (fd < 0)
        return;
    int stride = bitmap.pitch();
    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int>(stride) * bitmap.height());
    if (!pool) {
        close(fd);
        return;
    }
    auto format = bitmap.format() == Gfx::BitmapFormat::BGRA8888 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    auto* wl_buf = wl_shm_pool_create_buffer(pool, 0, bitmap.width(), bitmap.height(), stride, format);
    if (!wl_buf) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }
    wl_surface_attach(surface, wl_buf, 0, 0);
    wl_surface_damage(surface, 0, 0, bitmap.width(), bitmap.height());
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
    if (auto* window_surface = find(window_id)) {
        window_surface->inset = inset;
        window_surface->draw_inset = move(draw);
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
}

void WaylandClient::create_popup(i32 popup_id, i32 parent_window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu)
{
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
            auto local = anchor.location() - parent_window->requested_position;
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
        if (!m_screen_size.is_empty()) {
            if (menu_y + size.height() > m_screen_size.height())
                menu_y -= size.height() + max(0, anchor.height() - 1);
            menu_x = clamp(menu_x, 0, max(0, m_screen_size.width() - size.width()));
            menu_y = clamp(menu_y, 0, max(0, m_screen_size.height() - size.height()));
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
    if (!is_submenu && m_seat)
        xdg_popup_grab(popup, m_seat, m_last_input_serial);
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
}

void WaylandClient::destroy_popup(i32 popup_id)
{
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
    for (auto const& it : m_popups) {
        if (it.value->popup == popup) {
            if (m_input.popup_closed)
                m_input.popup_closed(it.value->id);
            return;
        }
    }
}

ErrorOr<ByteBuffer> WaylandClient::read_clipboard(ByteString& out_mime_type)
{
    out_mime_type = { };
    if (!m_current_offer || !m_display)
        return Error::from_string_literal("LibWM: no clipboard selection");

    ByteString mime = preferred_mime_for(m_current_offer);
    if (mime.is_empty())
        return Error::from_string_literal("LibWM: selection offers no usable type");

    int fds[2];
    if (pipe(fds) != 0)
        return Error::from_errno(errno);

    wl_data_offer_receive(m_current_offer, mime.characters(), fds[1]);
    wl_display_flush(m_display);
    close(fds[1]);

    ByteBuffer data;
    char buffer[4096];
    for (;;) {
        struct pollfd pfd {
            .fd = fds[0],
            .events = POLLIN,
            .revents = 0,
        };
        int rc = poll(&pfd, 1, 2000);
        if (rc <= 0)
            break;
        ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n <= 0)
            break;
        if (auto appended = data.try_append(buffer, static_cast<size_t>(n)); appended.is_error()) {
            close(fds[0]);
            return appended.release_error();
        }
    }
    close(fds[0]);

    if (mime.starts_with("text/plain"sv))
        mime = ByteString("text/plain");
    out_mime_type = move(mime);
    return data;
}

void WaylandClient::write_clipboard(HashMap<ByteString, ByteBuffer> offers)
{
    m_clipboard_offers = move(offers);

    if (!m_data_device_manager || !m_data_device) {
        dbgln("LibWM/Wayland: cannot set clipboard (no data device)");
        return;
    }

    if (m_data_source) {
        wl_data_source_destroy(m_data_source);
        m_data_source = nullptr;
    }

    if (m_clipboard_offers.is_empty()) {
        wl_data_device_set_selection(m_data_device, nullptr, m_last_input_serial);
        wl_display_flush(m_display);
        return;
    }

    m_data_source = wl_data_device_manager_create_data_source(m_data_device_manager);
    wl_data_source_add_listener(m_data_source, &s_data_source_listener, this);
    for (auto const& it : m_clipboard_offers)
        wl_data_source_offer(m_data_source, it.key.characters());
    wl_data_device_set_selection(m_data_device, m_data_source, m_last_input_serial);
    wl_display_flush(m_display);
    dbgln("LibWM/Wayland: set clipboard ({} representation(s), serial {})", m_clipboard_offers.size(), m_last_input_serial);
}

WaylandClient::WindowSurface* WaylandClient::find(i32 window_id)
{
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return nullptr;
    return it->value.ptr();
}

void WaylandClient::create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize size, ByteString const& title, bool has_alpha, bool resizable, i32 window_type)
{
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
            // Applets sit in the top layer, anchored to the bottom-right.
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            layer_namespace = "serenity-applet";
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
            window_surface->layer_output_position = { 0, m_screen_size.height() - size.height() };
            break;
        case WindowServer::WindowType::Applet:
            window_surface->layer_output_position = { m_screen_size.width() - size.width(), m_screen_size.height() - size.height() };
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

        m_windows.set(window_id, move(window_surface));
        wl_display_flush(m_display);
        return;
    }

    auto* surface = wl_compositor_create_surface(m_compositor);
    auto* xdg_surface_object = xdg_wm_base_get_xdg_surface(m_wm_base, surface);
    xdg_surface_add_listener(xdg_surface_object, &s_xdg_surface_listener, this);
    auto* toplevel = xdg_surface_get_toplevel(xdg_surface_object);
    xdg_toplevel_add_listener(toplevel, &s_toplevel_listener, this);
    xdg_toplevel_set_title(toplevel, title.characters());
    xdg_toplevel_set_app_id(toplevel, "libwm");
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
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return;
    auto& window_surface = *it->value;
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
    if (window_surface.surface)
        wl_surface_destroy(window_surface.surface);
    m_windows.remove(it);
}

void WaylandClient::set_title(i32 window_id, ByteString const& title)
{
    if (auto* window_surface = find(window_id)) {
        window_surface->title = title;
        if (window_surface->toplevel)
            xdg_toplevel_set_title(window_surface->toplevel, title.characters());
        wl_display_flush(m_display);
    }
}

void WaylandClient::set_window_rect(i32 window_id, Gfx::IntSize size)
{
    auto* window_surface = find(window_id);
    if (!window_surface || !window_surface->is_layer || !window_surface->layer_surface)
        return;

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
    if (auto* window_surface = find(window_id)) {
        if (!window_surface->toplevel)
            return;
        xdg_toplevel_set_minimized(window_surface->toplevel);
        wl_display_flush(m_display);
    }
}

void WaylandClient::purge_released_buffers(WindowSurface& window_surface)
{
    Vector<NonnullOwnPtr<BufferRecord>> kept;
    for (auto& record : window_surface.buffers) {
        if (record->released) {
            wl_buffer_destroy(record->buffer);
        } else {
            kept.append(move(record));
        }
    }
    window_surface.buffers = move(kept);
}

void WaylandClient::attach_and_commit(i32 window_id, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha)
{
    auto* window_surface = find(window_id);
    if (!window_surface || !m_shm || size.is_empty())
        return;
    if (visible_size.is_empty())
        visible_size = size;

    // A layer surface must not attach a buffer before its first configure; hold
    // the frame and present it from on_layer_configure().
    if (window_surface->is_layer && !window_surface->layer_configured) {
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

void WaylandClient::commit_window_content(WindowSurface& window_surface, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha)
{
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

    // If the window has a top inset (menubar), compose it above the client's
    // content into a single buffer. Otherwise attach the client's buffer as-is.
    if (window_surface.inset > 0 && window_surface.draw_inset) {
        int total_height = visible_size.height() + window_surface.inset;
        size_t bytes = static_cast<size_t>(visible_size.width()) * 4 * static_cast<size_t>(total_height);
        auto buffer = Core::AnonymousBuffer::create_with_size(bytes);
        if (buffer.is_error())
            return;
        window_surface.composed_buffer = buffer.release_value();
        auto composed = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, window_surface.composed_buffer, { visible_size.width(), total_height }, 1);
        if (composed.is_error())
            return;
        window_surface.composed_bitmap = composed.release_value();
        auto& destination = *window_surface.composed_bitmap;

        size_t client_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(size.height());
        auto* source = static_cast<u8 const*>(mmap(nullptr, client_bytes, PROT_READ, MAP_SHARED, client_fd, 0));
        if (source == MAP_FAILED)
            return;
        for (int y = 0; y < visible_size.height(); ++y)
            memcpy(destination.scanline(window_surface.inset + y), source + static_cast<size_t>(y) * pitch, static_cast<size_t>(visible_size.width()) * 4);
        munmap(const_cast<u8*>(source), client_bytes);

        window_surface.draw_inset(destination, { 0, 0, visible_size.width(), window_surface.inset });

        request_frame();
        bind_bitmap(window_surface.surface, window_surface.composed_buffer, destination, window_surface.buffers);
        wl_surface_commit(window_surface.surface);
        wl_display_flush(m_display);
        dbgln("LibWM/Wayland: presented window {} ({}x{} incl. {}px inset)", window_surface.window_id, visible_size.width(), total_height, window_surface.inset);
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
    // Present only the visible region; the extra rows/columns of the widened
    // backing store are simply not referenced.
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, visible_size.width(), visible_size.height(), pitch, format);
    if (!buffer) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }

    request_frame();
    wl_surface_attach(window_surface.surface, buffer, 0, 0);
    wl_surface_damage(window_surface.surface, 0, 0, visible_size.width(), visible_size.height());
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
    for (auto const& it : m_windows) {
        auto& window_surface = *it.value;
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
