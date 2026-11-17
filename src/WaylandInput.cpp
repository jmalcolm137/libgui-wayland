/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <Kernel/API/KeyCode.h>
#include <LibGUI/Event.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "text-input-unstable-v3-client-protocol.h"

namespace LibWM {

// --- zwp_text_input_v3 --------------------------------------------------------

static void text_input_enter(void* data, zwp_text_input_v3*, wl_surface*)
{
    static_cast<WaylandClient*>(data)->on_text_input_enter();
}

static void text_input_leave(void* data, zwp_text_input_v3*, wl_surface*)
{
    (void)data;
}

static void text_input_preedit_string(void* data, zwp_text_input_v3*, char const* text, int32_t cursor_begin, int32_t cursor_end)
{
    auto& self = *static_cast<WaylandClient*>(data);
    self.on_text_input_preedit(text ? String::from_utf8_with_replacement_character(StringView { text, strlen(text) }) : String {}, cursor_begin, cursor_end);
}

static void text_input_commit_string(void* data, zwp_text_input_v3*, char const* text)
{
    auto& self = *static_cast<WaylandClient*>(data);
    if (text)
        self.on_text_input_commit(String::from_utf8_with_replacement_character(StringView { text, strlen(text) }));
}

static void text_input_delete_surrounding_text(void* data, zwp_text_input_v3*, uint32_t before_length, uint32_t after_length)
{
    auto& self = *static_cast<WaylandClient*>(data);
    self.on_text_input_delete_surrounding(static_cast<i32>(before_length), static_cast<i32>(after_length));
}

static void text_input_done(void*, zwp_text_input_v3*, uint32_t) { }
static void text_input_action(void*, zwp_text_input_v3*, uint32_t, uint32_t) { }
static void text_input_language(void*, zwp_text_input_v3*, char const*) { }
static void text_input_preedit_hint(void*, zwp_text_input_v3*, uint32_t, uint32_t, uint32_t) { }

static zwp_text_input_v3_listener const s_text_input_listener = {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit_string,
    .commit_string = text_input_commit_string,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .done = text_input_done,
    .action = text_input_action,
    .language = text_input_language,
    .preedit_hint = text_input_preedit_hint,
};

void WaylandClient::on_text_input_enter()
{
    MutexLocker locker(m_mutex);
    if (!m_text_input.enabled || !m_text_input.device)
        return;
    // A field enabled before the input method connected is reactivated when the compositor reports the new focus.
    zwp_text_input_v3_enable(m_text_input.device);
    zwp_text_input_v3_set_content_type(m_text_input.device, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    if (!m_text_input.cursor_rect.is_empty())
        zwp_text_input_v3_set_cursor_rectangle(m_text_input.device, m_text_input.cursor_rect.x(), m_text_input.cursor_rect.y(), m_text_input.cursor_rect.width(), m_text_input.cursor_rect.height());
    zwp_text_input_v3_commit(m_text_input.device);
    if (m_display)
        wl_display_flush(m_display);
}

void WaylandClient::on_text_input_commit(String text)
{
    MutexLocker locker(m_mutex);
    if (m_text_input.on_commit)
        m_text_input.on_commit(text);
}

void WaylandClient::on_text_input_preedit(String text, i32 cursor_begin, i32 cursor_end)
{
    MutexLocker locker(m_mutex);
    if (m_text_input.on_preedit)
        m_text_input.on_preedit(text, cursor_begin, cursor_end);
}

void WaylandClient::on_text_input_delete_surrounding(i32 before_length, i32 after_length)
{
    MutexLocker locker(m_mutex);
    if (m_text_input.on_delete_surrounding)
        m_text_input.on_delete_surrounding(before_length, after_length);
}

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
    double delta = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        static_cast<WaylandClient*>(data)->on_pointer_axis_delta(0, delta);
    else
        static_cast<WaylandClient*>(data)->on_pointer_axis_delta(delta, 0);
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

void WaylandClient::add_seat(wl_seat* seat)
{
    MutexLocker locker(m_mutex);
    m_input_state.seat = seat;
    wl_seat_add_listener(seat, &s_seat_listener, this);
    maybe_create_data_device();
    ensure_text_input();
}

void WaylandClient::set_text_input_manager(zwp_text_input_manager_v3* manager)
{
    MutexLocker locker(m_mutex);
    m_text_input.manager = manager;
    ensure_text_input();
}

void WaylandClient::ensure_text_input()
{
    MutexLocker locker(m_mutex);
    if (m_text_input.device || !m_text_input.manager || !m_input_state.seat)
        return;
    m_text_input.device = zwp_text_input_manager_v3_get_text_input(m_text_input.manager, m_input_state.seat);
    if (m_text_input.device)
        zwp_text_input_v3_add_listener(m_text_input.device, &s_text_input_listener, this);
}

void WaylandClient::set_text_input_callbacks(TextInputCommit commit, TextInputPreedit preedit, TextInputDeleteSurrounding delete_surrounding)
{
    MutexLocker locker(m_mutex);
    m_text_input.on_commit = move(commit);
    m_text_input.on_preedit = move(preedit);
    m_text_input.on_delete_surrounding = move(delete_surrounding);
}

void WaylandClient::set_text_input_enabled(bool enabled)
{
    MutexLocker locker(m_mutex);
    if (m_text_input.enabled == enabled)
        return;
    m_text_input.enabled = enabled;
    if (!m_text_input.device)
        return;

    if (enabled) {
        zwp_text_input_v3_enable(m_text_input.device);
        zwp_text_input_v3_set_content_type(m_text_input.device, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        if (!m_text_input.cursor_rect.is_empty())
            zwp_text_input_v3_set_cursor_rectangle(m_text_input.device, m_text_input.cursor_rect.x(), m_text_input.cursor_rect.y(), m_text_input.cursor_rect.width(), m_text_input.cursor_rect.height());
    } else {
        zwp_text_input_v3_disable(m_text_input.device);
    }
    zwp_text_input_v3_commit(m_text_input.device);
    if (m_display)
        wl_display_flush(m_display);
}

void WaylandClient::set_text_input_cursor_rect(Gfx::IntRect const& rect)
{
    MutexLocker locker(m_mutex);
    if (m_text_input.cursor_rect == rect)
        return;
    m_text_input.cursor_rect = rect;
    if (!m_text_input.device || !m_text_input.enabled)
        return;
    zwp_text_input_v3_set_cursor_rectangle(m_text_input.device, rect.x(), rect.y(), rect.width(), rect.height());
    zwp_text_input_v3_commit(m_text_input.device);
    if (m_display)
        wl_display_flush(m_display);
}

void WaylandClient::on_seat_capabilities(u32 capabilities)
{
    MutexLocker locker(m_mutex);
    // A capability that goes away takes the device object with it: the
    // compositor makes it inert, and it never delivers another event. Drop it, so
    // that the capability coming back (a keyboard plugged in again) gets a fresh
    // one, with its keymap and enter.
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && m_input_state.pointer) {
        wl_pointer_destroy(m_input_state.pointer);
        m_input_state.pointer = nullptr;
        m_input_state.pointer_popup = -1;
        m_input_state.pointer_window = -1;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && m_input_state.keyboard) {
        wl_keyboard_destroy(m_input_state.keyboard);
        m_input_state.keyboard = nullptr;
        m_input_state.focused_window = -1;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !m_input_state.pointer) {
        m_input_state.pointer = wl_seat_get_pointer(m_input_state.seat);
        wl_pointer_add_listener(m_input_state.pointer, &s_pointer_listener, this);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !m_input_state.keyboard) {
        m_input_state.keyboard = wl_seat_get_keyboard(m_input_state.seat);
        wl_keyboard_add_listener(m_input_state.keyboard, &s_keyboard_listener, this);
    }
    dbgln("LibWM/Wayland: seat capabilities 0x{:x} (pointer={}, keyboard={})", capabilities,
        m_input_state.pointer != nullptr, m_input_state.keyboard != nullptr);
}

void WaylandClient::on_pointer_enter(wl_surface* surface, Gfx::IntPoint position)
{
    MutexLocker locker(m_mutex);
    m_input_state.pointer_popup = popup_id_for_surface(surface);
    m_input_state.pointer_window = m_input_state.pointer_popup >= 0 ? -1 : window_id_for_surface(surface);
    m_input_state.pointer_surface = surface;
    m_input_state.pointer_position = position;
    dbgln("LibWM/Wayland: pointer entered {} {} at {},{}",
        m_input_state.pointer_popup >= 0 ? "popup" : "window",
        m_input_state.pointer_popup >= 0 ? m_input_state.pointer_popup : m_input_state.pointer_window,
        position.x(), position.y());

    if (m_input_state.pointer_popup >= 0) {
        if (m_input.popup_motion)
            m_input.popup_motion(m_input_state.pointer_popup, position);
        return;
    }
    if (m_input_state.pointer_window >= 0 && m_input.window_entered)
        m_input.window_entered(m_input_state.pointer_window);
}

void WaylandClient::on_pointer_leave()
{
    MutexLocker locker(m_mutex);
    if (m_input_state.pointer_popup >= 0) {
        m_input_state.pointer_popup = -1;
        m_input_state.pointer_window = -1;
        return;
    }
    if (m_input_state.pointer_window >= 0 && m_input.menubar_left)
        m_input.menubar_left(m_input_state.pointer_window);
    if (m_input_state.pointer_window >= 0 && m_input.window_left)
        m_input.window_left(m_input_state.pointer_window);
    m_input_state.pointer_window = -1;
    m_input_state.pointer_surface = nullptr;
    m_input_state.axis_accum_x = 0;
    m_input_state.axis_accum_y = 0;
}

void WaylandClient::on_pointer_motion(Gfx::IntPoint position)
{
    MutexLocker locker(m_mutex);
    m_input_state.pointer_position = position;

    if (m_input_state.pointer_popup >= 0) {
        if (m_input.popup_motion)
            m_input.popup_motion(m_input_state.pointer_popup, position);
        return;
    }
    if (m_input_state.pointer_window < 0)
        return;

    auto* window_surface = find(m_input_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_input_state.pointer_surface == window_surface->menubar_surface) {
            if (m_input.menubar_motion)
                m_input.menubar_motion(m_input_state.pointer_window, position);
            return;
        }
        if (m_input.menubar_left)
            m_input.menubar_left(m_input_state.pointer_window);
        if (m_input.mouse_move)
            m_input.mouse_move(m_input_state.pointer_window, position, m_input_state.pointer_buttons, current_modifiers());
        return;
    }
    int inset = window_surface ? window_surface->inset : 0;
    if (inset > 0 && position.y() < inset) {
        if (m_input.menubar_motion)
            m_input.menubar_motion(m_input_state.pointer_window, position);
        return;
    }
    if (m_input.menubar_left)
        m_input.menubar_left(m_input_state.pointer_window);
    if (m_input.mouse_move)
        m_input.mouse_move(m_input_state.pointer_window, position.translated(0, -inset), m_input_state.pointer_buttons, current_modifiers());
}

void WaylandClient::on_pointer_button(u32 button, bool pressed)
{
    MutexLocker locker(m_mutex);
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

    if (m_input_state.pointer_popup >= 0) {
        if (m_input.popup_button)
            m_input.popup_button(m_input_state.pointer_popup, m_input_state.pointer_position, pressed);
        return;
    }
    if (m_input_state.pointer_window < 0)
        return;

    auto* window_surface = find(m_input_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_input_state.pointer_surface == window_surface->menubar_surface) {
            if (pressed && m_input.menubar_press)
                m_input.menubar_press(m_input_state.pointer_window, m_input_state.pointer_position);
            return;
        }
        auto position = m_input_state.pointer_position;
        if (pressed) {
            m_input_state.pointer_buttons |= serenity_button;
        } else {
            m_input_state.pointer_buttons &= ~serenity_button;
        }
        if (pressed) {
            if (m_input.mouse_down)
                m_input.mouse_down(m_input_state.pointer_window, position, serenity_button, m_input_state.pointer_buttons, current_modifiers());
        } else {
            if (m_input.mouse_up)
                m_input.mouse_up(m_input_state.pointer_window, position, serenity_button, m_input_state.pointer_buttons, current_modifiers());
        }
        return;
    }
    int inset = window_surface ? window_surface->inset : 0;
    if (inset > 0 && m_input_state.pointer_position.y() < inset) {
        if (pressed && m_input.menubar_press)
            m_input.menubar_press(m_input_state.pointer_window, m_input_state.pointer_position);
        return;
    }

    auto position = m_input_state.pointer_position.translated(0, -inset);
    if (pressed) {
        m_input_state.pointer_buttons |= serenity_button;
        dbgln("LibWM/Wayland: mouse down window {} at {},{} button={}", m_input_state.pointer_window, position.x(), position.y(), serenity_button);
        if (m_input.mouse_down)
            m_input.mouse_down(m_input_state.pointer_window, position, serenity_button, m_input_state.pointer_buttons, current_modifiers());
    } else {
        m_input_state.pointer_buttons &= ~serenity_button;
        dbgln("LibWM/Wayland: mouse up window {} at {},{} button={}", m_input_state.pointer_window, position.x(), position.y(), serenity_button);
        if (m_input.mouse_up)
            m_input.mouse_up(m_input_state.pointer_window, position, serenity_button, m_input_state.pointer_buttons, current_modifiers());
    }
}

void WaylandClient::on_pointer_axis(i32 x, i32 y)
{
    MutexLocker locker(m_mutex);
    if (m_input_state.pointer_popup >= 0) {
        if (y != 0 && m_input.popup_wheel)
            m_input.popup_wheel(m_input_state.pointer_popup, y);
        return;
    }
    if (m_input_state.pointer_window < 0)
        return;
    auto* window_surface = find(m_input_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_input_state.pointer_surface == window_surface->menubar_surface)
            return;
        if (m_input.mouse_wheel)
            m_input.mouse_wheel(m_input_state.pointer_window, m_input_state.pointer_position, m_input_state.pointer_buttons, current_modifiers(), x, y);
        return;
    }
    int inset = window_surface ? window_surface->inset : 0;
    if (m_input.mouse_wheel)
        m_input.mouse_wheel(m_input_state.pointer_window, m_input_state.pointer_position.translated(0, -inset), m_input_state.pointer_buttons, current_modifiers(), x, y);
}

void WaylandClient::on_pointer_axis_delta(double x, double y)
{
    MutexLocker locker(m_mutex);
    // Wayland delivers smooth axis values that are often fractional (trackpads
    // and high-resolution wheels). SerenityOS widgets expect whole scroll steps,
    // so accumulate and emit the integral part, carrying the remainder.
    m_input_state.axis_accum_x += x;
    m_input_state.axis_accum_y += y;
    auto step_x = static_cast<i32>(m_input_state.axis_accum_x);
    auto step_y = static_cast<i32>(m_input_state.axis_accum_y);
    if (getenv("LIBWM_AXIS_DEBUG"))
        dbgln("LibWM/Wayland: axis delta {}x{} -> step {}x{}", x, y, step_x, step_y);
    if (step_x == 0 && step_y == 0)
        return;
    m_input_state.axis_accum_x -= step_x;
    m_input_state.axis_accum_y -= step_y;
    on_pointer_axis(step_x, step_y);
}

void WaylandClient::on_keyboard_keymap(int fd, u32 size)
{
    MutexLocker locker(m_mutex);
    if (!m_input_state.xkb_context)
        m_input_state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    auto* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    close(fd);

    if (m_input_state.xkb_keymap)
        xkb_keymap_unref(m_input_state.xkb_keymap);
    m_input_state.xkb_keymap = xkb_keymap_new_from_string(m_input_state.xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);

    if (m_input_state.xkb_state)
        xkb_state_unref(m_input_state.xkb_state);
    m_input_state.xkb_state = m_input_state.xkb_keymap ? xkb_state_new(m_input_state.xkb_keymap) : nullptr;

    dbgln("LibWM/Wayland: received keymap ({} bytes), state={}", size, m_input_state.xkb_state != nullptr);
}

u32 WaylandClient::current_modifiers() const
{
    MutexLocker locker(m_mutex);
    return m_input_state.modifiers;
}

void WaylandClient::on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32)
{
    MutexLocker locker(m_mutex);
    // The compositor's modifier state is authoritative. Key events alone are not
    // enough: if a release is lost because focus moved while Ctrl/Alt was held,
    // the next click and wheel event would silently keep that modifier forever.
    if (!m_input_state.xkb_state) {
        dbgln("LibWM/Wayland: modifiers depressed={:#x} locked={:#x} group={}", depressed, locked, group);
        return;
    }

    xkb_state_update_mask(m_input_state.xkb_state, depressed, latched, locked, 0, 0, group);

    u32 modifiers = 0;
    if (xkb_state_mod_name_is_active(m_input_state.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Shift;
    if (xkb_state_mod_name_is_active(m_input_state.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Ctrl;
    if (xkb_state_mod_name_is_active(m_input_state.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Alt;
    if (xkb_state_mod_name_is_active(m_input_state.xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Super;
    m_input_state.modifiers = modifiers;

    dbgln("LibWM/Wayland: modifiers depressed={:#x} locked={:#x} group={} -> {:#x}", depressed, locked, group, m_input_state.modifiers);
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
    MutexLocker locker(m_mutex);
    if (!m_input_state.xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    // Update the xkb state from the key itself so code_point reflects
    // Shift/CapsLock/etc. even without modifier events.
    xkb_state_update_key(m_input_state.xkb_state, code, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    u32 code_point = xkb_state_key_get_utf32(m_input_state.xkb_state, code);
    u32 key_code = static_cast<u32>(serenity_key_code_from_evdev(key));

    // Serenity's WindowServer turned Ctrl+<letter> into the control character
    // itself, so a client sees Ctrl-C as code point 3 rather than 'c' with a ctrl
    // flag; LibVT relies on that, and so does every terminal program that expects a
    // SIGINT.
    if ((current_modifiers() & Mod_Ctrl) && code_point >= '@' && code_point < 0x7f)
        code_point &= 0x1f;

    if (u32 bit = modifier_bit_for_evdev(key); bit != 0) {
        if (pressed)
            m_input_state.modifiers |= bit;
        else
            m_input_state.modifiers &= ~bit;
    }

    if (m_input.key)
        m_input.key(m_input_state.focused_window, code_point, key_code, 0, current_modifiers(), key, pressed);
}

void WaylandClient::on_keyboard_focus(wl_surface* surface, bool entered)
{
    MutexLocker locker(m_mutex);
    m_input_state.focused_window = entered ? window_id_for_surface(surface) : -1;
}

Gfx::IntPoint WaylandClient::global_pointer_position()
{
    MutexLocker locker(m_mutex);
    // Popups (server-rendered menus) are layer surfaces with a known position.
    if (m_input_state.pointer_popup >= 0) {
        if (auto* popup = find_popup(m_input_state.pointer_popup))
            return popup->layer_position.translated(m_input_state.pointer_position);
    }
    if (m_input_state.pointer_window >= 0) {
        if (auto* window_surface = find(m_input_state.pointer_window)) {
            // Layer surfaces (Taskbar, Desktop) know their output position; for
            // toplevels it comes from the compositor's position event.
            auto origin = window_surface->is_layer ? window_surface->layer_output_position
                                                   : window_surface->global_position;
            return origin.translated(m_input_state.pointer_position);
        }
    }
    return m_input_state.pointer_position;
}

}
