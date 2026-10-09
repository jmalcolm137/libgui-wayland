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
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-decoration-client-protocol.h"
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

// --- xdg_surface_object --------------------------------------------------------------

static void xdg_surface_configure(void*, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static xdg_surface_listener const s_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// --- xdg_toplevel -------------------------------------------------------------

static void toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t, int32_t, wl_array* states)
{
    auto& self = *static_cast<WaylandClient*>(data);
    bool activated = false;
    auto* state_data = static_cast<uint32_t const*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        if (state_data[i] == XDG_TOPLEVEL_STATE_ACTIVATED)
            activated = true;
    }
    self.on_toplevel_configure(toplevel, activated);
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

// --- zxdg_toplevel_decoration_v1 ----------------------------------------------

static void decoration_configure(void*, zxdg_toplevel_decoration_v1*, uint32_t mode)
{
    dbgln("LibWM/Wayland: server-side decoration mode {}", mode);
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

// --- wl_seat / wl_pointer / wl_keyboard --------------------------------------

static KeyCode serenity_key_code_from_evdev(u32 key)
{
    switch (key) {
    case KEY_ESC: return Key_Escape;
    case KEY_TAB: return Key_Tab;
    case KEY_BACKSPACE: return Key_Backspace;
    case KEY_ENTER: return Key_Return;
    case KEY_INSERT: return Key_Insert;
    case KEY_DELETE: return Key_Delete;
    case KEY_HOME: return Key_Home;
    case KEY_END: return Key_End;
    case KEY_LEFT: return Key_Left;
    case KEY_UP: return Key_Up;
    case KEY_RIGHT: return Key_Right;
    case KEY_DOWN: return Key_Down;
    case KEY_PAGEUP: return Key_PageUp;
    case KEY_PAGEDOWN: return Key_PageDown;
    case KEY_LEFTSHIFT: return Key_LeftShift;
    case KEY_RIGHTSHIFT: return Key_RightShift;
    case KEY_LEFTCTRL: return Key_LeftControl;
    case KEY_RIGHTCTRL: return Key_RightControl;
    case KEY_LEFTALT: return Key_LeftAlt;
    case KEY_RIGHTALT: return Key_RightAlt;
    case KEY_LEFTMETA: return Key_LeftSuper;
    case KEY_RIGHTMETA: return Key_RightSuper;
    case KEY_CAPSLOCK: return Key_CapsLock;
    case KEY_NUMLOCK: return Key_NumLock;
    case KEY_SCROLLLOCK: return Key_ScrollLock;
    case KEY_SPACE: return Key_Space;
    case KEY_MINUS: return Key_Minus;
    case KEY_EQUAL: return Key_Equal;
    case KEY_LEFTBRACE: return Key_LeftBracket;
    case KEY_RIGHTBRACE: return Key_RightBracket;
    case KEY_BACKSLASH: return Key_Backslash;
    case KEY_SEMICOLON: return Key_Semicolon;
    case KEY_APOSTROPHE: return Key_Apostrophe;
    case KEY_GRAVE: return Key_Backtick;
    case KEY_COMMA: return Key_Comma;
    case KEY_DOT: return Key_Period;
    case KEY_SLASH: return Key_Slash;
    case KEY_1: return Key_1;
    case KEY_2: return Key_2;
    case KEY_3: return Key_3;
    case KEY_4: return Key_4;
    case KEY_5: return Key_5;
    case KEY_6: return Key_6;
    case KEY_7: return Key_7;
    case KEY_8: return Key_8;
    case KEY_9: return Key_9;
    case KEY_0: return Key_0;
    case KEY_F1: return Key_F1;
    case KEY_F2: return Key_F2;
    case KEY_F3: return Key_F3;
    case KEY_F4: return Key_F4;
    case KEY_F5: return Key_F5;
    case KEY_F6: return Key_F6;
    case KEY_F7: return Key_F7;
    case KEY_F8: return Key_F8;
    case KEY_F9: return Key_F9;
    case KEY_F10: return Key_F10;
    case KEY_F11: return Key_F11;
    case KEY_F12: return Key_F12;
    case KEY_A: return Key_A;
    case KEY_B: return Key_B;
    case KEY_C: return Key_C;
    case KEY_D: return Key_D;
    case KEY_E: return Key_E;
    case KEY_F: return Key_F;
    case KEY_G: return Key_G;
    case KEY_H: return Key_H;
    case KEY_I: return Key_I;
    case KEY_J: return Key_J;
    case KEY_K: return Key_K;
    case KEY_L: return Key_L;
    case KEY_M: return Key_M;
    case KEY_N: return Key_N;
    case KEY_O: return Key_O;
    case KEY_P: return Key_P;
    case KEY_Q: return Key_Q;
    case KEY_R: return Key_R;
    case KEY_S: return Key_S;
    case KEY_T: return Key_T;
    case KEY_U: return Key_U;
    case KEY_V: return Key_V;
    case KEY_W: return Key_W;
    case KEY_X: return Key_X;
    case KEY_Y: return Key_Y;
    case KEY_Z: return Key_Z;
    default: return Key_Invalid;
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
        m_screen_size.width(), m_screen_size.height(), m_scale, m_scale);
    return {};
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
    wl_output_add_listener(output, &s_output_listener, this);
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

void WaylandClient::on_output_done()
{
    if (m_physical_size.is_empty())
        return;
    // Report the logical size to SerenityOS; physical pixels stay with the
    // compositor and the output scale (principle P1).
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
    m_pointer_window = window_id_for_surface(surface);
    m_pointer_position = position;
    dbgln("LibWM/Wayland: pointer entered window {} at {},{}", m_pointer_window, position.x(), position.y());
    if (m_pointer_window >= 0 && m_input.window_entered)
        m_input.window_entered(m_pointer_window);
}

void WaylandClient::on_pointer_leave()
{
    if (m_pointer_window >= 0 && m_input.window_left)
        m_input.window_left(m_pointer_window);
    m_pointer_window = -1;
}

void WaylandClient::on_pointer_motion(Gfx::IntPoint position)
{
    m_pointer_position = position;
    if (m_pointer_window >= 0 && m_input.mouse_move)
        m_input.mouse_move(m_pointer_window, position, m_pointer_buttons, current_modifiers());
}

void WaylandClient::on_pointer_button(u32 button, bool pressed)
{
    if (m_pointer_window < 0)
        return;

    u32 serenity_button = 0;
    switch (button) {
    case BTN_LEFT: serenity_button = static_cast<u32>(GUI::MouseButton::Primary); break;
    case BTN_RIGHT: serenity_button = static_cast<u32>(GUI::MouseButton::Secondary); break;
    case BTN_MIDDLE: serenity_button = static_cast<u32>(GUI::MouseButton::Middle); break;
    case BTN_SIDE: serenity_button = static_cast<u32>(GUI::MouseButton::Backward); break;
    case BTN_EXTRA: serenity_button = static_cast<u32>(GUI::MouseButton::Forward); break;
    default: return;
    }

    if (pressed) {
        m_pointer_buttons |= serenity_button;
        dbgln("LibWM/Wayland: mouse down window {} at {},{} button={}", m_pointer_window, m_pointer_position.x(), m_pointer_position.y(), serenity_button);
        if (m_input.mouse_down)
            m_input.mouse_down(m_pointer_window, m_pointer_position, serenity_button, m_pointer_buttons, current_modifiers());
    } else {
        m_pointer_buttons &= ~serenity_button;
        dbgln("LibWM/Wayland: mouse up window {} at {},{} button={}", m_pointer_window, m_pointer_position.x(), m_pointer_position.y(), serenity_button);
        if (m_input.mouse_up)
            m_input.mouse_up(m_pointer_window, m_pointer_position, serenity_button, m_pointer_buttons, current_modifiers());
    }
}

void WaylandClient::on_pointer_axis(i32 x, i32 y)
{
    if (m_pointer_window < 0)
        return;
    if (m_input.mouse_wheel)
        m_input.mouse_wheel(m_pointer_window, m_pointer_position, m_pointer_buttons, current_modifiers(), x, y);
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
    if (!m_xkb_state)
        return 0;
    u32 modifiers = 0;
    if (xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Shift;
    if (xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Ctrl;
    if (xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Alt;
    if (xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Super;
    if (xkb_state_mod_name_is_active(m_xkb_state, "Level3", XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_AltGr;
    return modifiers;
}

void WaylandClient::on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32)
{
    if (m_xkb_state)
        xkb_state_update_mask(m_xkb_state, depressed, latched, locked, 0, 0, group);
}

void WaylandClient::on_keyboard_key(u32 key, bool pressed)
{
    if (m_focused_window < 0 || !m_xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    u32 code_point = xkb_state_key_get_utf32(m_xkb_state, code);
    u32 key_code = static_cast<u32>(serenity_key_code_from_evdev(key));
    dbgln("LibWM/Wayland: key {} evdev={} code_point={} key_code={} pressed={}", m_focused_window, key, code_point, key_code, pressed);

    if (m_input.key)
        m_input.key(m_focused_window, code_point, key_code, 0, current_modifiers(), key, pressed);
}

void WaylandClient::on_keyboard_focus(wl_surface* surface, bool entered)
{
    m_focused_window = entered ? window_id_for_surface(surface) : -1;
}

void WaylandClient::on_toplevel_configure(xdg_toplevel* toplevel, bool activated)
{
    for (auto const& it : m_windows) {
        if (it.value->toplevel == toplevel) {
            if (m_input.window_activation)
                m_input.window_activation(it.value->window_id, activated);
            return;
        }
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

ByteString WaylandClient::preferred_mime_for(wl_data_offer* offer) const
{
    auto it = m_offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it == m_offer_mime_types.end() || it->value.is_empty())
        return {};
    auto const& mimes = it->value;
    for (auto const& candidate : { "text/plain;charset=utf-8"sv, "text/plain"sv, "UTF8_STRING"sv, "STRING"sv }) {
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
    m_offer_mime_types.set(reinterpret_cast<u64>(offer), {});
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

void WaylandClient::on_source_send(wl_data_source*, char const*, int fd)
{
    if (!m_clipboard_data.is_empty()) {
        size_t written = 0;
        while (written < m_clipboard_data.size()) {
            ssize_t n = write(fd, m_clipboard_data.data() + written, m_clipboard_data.size() - written);
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

ErrorOr<ByteBuffer> WaylandClient::read_clipboard(ByteString& out_mime_type)
{
    out_mime_type = {};
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

void WaylandClient::write_clipboard(ReadonlyBytes data, ByteString const& mime_type)
{
    if (auto copied = ByteBuffer::copy(data); !copied.is_error())
        m_clipboard_data = copied.release_value();
    else
        return;
    m_clipboard_mime_type = mime_type;

    if (!m_data_device_manager || !m_data_device) {
        dbgln("LibWM/Wayland: cannot set clipboard (no data device)");
        return;
    }

    if (m_data_source) {
        wl_data_source_destroy(m_data_source);
        m_data_source = nullptr;
    }

    m_data_source = wl_data_device_manager_create_data_source(m_data_device_manager);
    wl_data_source_add_listener(m_data_source, &s_data_source_listener, this);
    wl_data_source_offer(m_data_source, mime_type.characters());
    if (mime_type == "text/plain"sv) {
        wl_data_source_offer(m_data_source, "text/plain;charset=utf-8");
        wl_data_source_offer(m_data_source, "UTF8_STRING");
        wl_data_source_offer(m_data_source, "STRING");
    }
    wl_data_device_set_selection(m_data_device, m_data_source, m_last_input_serial);
    wl_display_flush(m_display);
    dbgln("LibWM/Wayland: set clipboard (mime='{}', {} bytes, serial {})", mime_type, data.size(), m_last_input_serial);
}

WaylandClient::WindowSurface* WaylandClient::find(i32 window_id)
{
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return nullptr;
    return it->value.ptr();
}

void WaylandClient::create_window(i32 window_id, Gfx::IntSize size, ByteString const& title, bool has_alpha)
{
    if (!m_compositor || !m_wm_base)
        return;

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
    if (window_surface.toplevel)
        xdg_toplevel_destroy(window_surface.toplevel);
    if (window_surface.decoration)
        zxdg_toplevel_decoration_v1_destroy(window_surface.decoration);
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
        xdg_toplevel_set_title(window_surface->toplevel, title.characters());
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

void WaylandClient::attach_and_commit(i32 window_id, int client_fd, Gfx::IntSize size, i32 pitch, bool has_alpha)
{
    auto* window_surface = find(window_id);
    if (!window_surface || !m_shm || size.is_empty())
        return;

    purge_released_buffers(*window_surface);

    int fd = dup(client_fd);
    if (fd < 0)
        return;

    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int>(pitch) * size.height());
    if (!pool) {
        close(fd);
        return;
    }

    auto format = has_alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, size.width(), size.height(), pitch, format);
    if (!buffer) {
        wl_shm_pool_destroy(pool);
        close(fd);
        return;
    }

    wl_surface_attach(window_surface->surface, buffer, 0, 0);
    wl_surface_damage(window_surface->surface, 0, 0, size.width(), size.height());
    wl_surface_commit(window_surface->surface);
    wl_display_flush(m_display);

    // The pool's fd has been transferred by the flush above; buffers outlive it.
    wl_shm_pool_destroy(pool);
    close(fd);

    auto record = make<BufferRecord>();
    record->buffer = buffer;
    wl_buffer_add_listener(buffer, &s_buffer_listener, record.ptr());
    window_surface->buffers.append(move(record));

    dbgln("LibWM/Wayland: presented window {} ({}x{})", window_id, size.width(), size.height());
}

}
