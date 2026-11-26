/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandSeat.h"
#include "WaylandClient.h"
#include "SerenityShell.h"
#include <LibCore/Timer.h>
#include <AK/Time.h>
#include "WaylandDataDevice.h"
#include "Environment.h"
#include <Kernel/API/KeyCode.h>
#include <LibGUI/Event.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CursorParams.h>
#include <LibGfx/StandardCursor.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace LibWM {

using MutexLocker = WaylandClient::MutexLocker;

// --- zwp_text_input_v3 --------------------------------------------------------

static void text_input_enter(void* data, zwp_text_input_v3*, wl_surface*)
{
    static_cast<WaylandSeat*>(data)->on_text_input_enter();
}

static void text_input_leave(void* data, zwp_text_input_v3*, wl_surface*)
{
    (void)data;
}

static void text_input_preedit_string(void* data, zwp_text_input_v3*, char const* text, int32_t cursor_begin, int32_t cursor_end)
{
    auto& self = *static_cast<WaylandSeat*>(data);
    self.on_text_input_preedit(text ? String::from_utf8_with_replacement_character(StringView { text, strlen(text) }) : String {}, cursor_begin, cursor_end);
}

static void text_input_commit_string(void* data, zwp_text_input_v3*, char const* text)
{
    auto& self = *static_cast<WaylandSeat*>(data);
    if (text)
        self.on_text_input_commit(String::from_utf8_with_replacement_character(StringView { text, strlen(text) }));
}

static void text_input_delete_surrounding_text(void* data, zwp_text_input_v3*, uint32_t before_length, uint32_t after_length)
{
    auto& self = *static_cast<WaylandSeat*>(data);
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

void WaylandSeat::on_text_input_enter()
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_text_input.enabled || !m_text_input.device)
        return;
    // A field enabled before the input method connected is reactivated when the compositor reports the new focus.
    zwp_text_input_v3_enable(m_text_input.device);
    zwp_text_input_v3_set_content_type(m_text_input.device, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    if (!m_text_input.cursor_rect.is_empty())
        zwp_text_input_v3_set_cursor_rectangle(m_text_input.device, m_text_input.cursor_rect.x(), m_text_input.cursor_rect.y(), m_text_input.cursor_rect.width(), m_text_input.cursor_rect.height());
    zwp_text_input_v3_commit(m_text_input.device);
    if (m_client.m_display)
        wl_display_flush(m_client.m_display);
}

void WaylandSeat::on_text_input_commit(String text)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_text_input.on_commit)
        m_text_input.on_commit(text);
}

void WaylandSeat::on_text_input_preedit(String text, i32 cursor_begin, i32 cursor_end)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_text_input.on_preedit)
        m_text_input.on_preedit(text, cursor_begin, cursor_end);
}

void WaylandSeat::on_text_input_delete_surrounding(i32 before_length, i32 after_length)
{
    MutexLocker locker(m_client.m_mutex);
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
    static_cast<WaylandSeat*>(data)->on_seat_capabilities(capabilities);
}

static void seat_name(void*, wl_seat*, char const*) { }

static wl_seat_listener const s_seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void pointer_enter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandSeat*>(data)->note_pointer_enter_serial(serial);
    static_cast<WaylandSeat*>(data)->on_pointer_enter(surface, { wl_fixed_to_int(sx), wl_fixed_to_int(sy) });
}

static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*)
{
    static_cast<WaylandSeat*>(data)->on_pointer_leave();
}

static void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandSeat*>(data)->on_pointer_motion({ wl_fixed_to_int(sx), wl_fixed_to_int(sy) });
}

static void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
    static_cast<WaylandSeat*>(data)->note_input_serial(serial);
    static_cast<WaylandSeat*>(data)->on_pointer_button(button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value)
{
    double delta = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        static_cast<WaylandSeat*>(data)->on_pointer_axis_delta(0, delta);
    else
        static_cast<WaylandSeat*>(data)->on_pointer_axis_delta(delta, 0);
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
    static_cast<WaylandSeat*>(data)->on_keyboard_keymap(fd, size);
}

static void keyboard_enter(void* data, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array*)
{
    static_cast<WaylandSeat*>(data)->note_input_serial(serial);
    static_cast<WaylandSeat*>(data)->on_keyboard_focus(surface, true);
}

static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*)
{
    static_cast<WaylandSeat*>(data)->on_keyboard_focus(nullptr, false);
}

static void keyboard_key(void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
{
    static_cast<WaylandSeat*>(data)->note_input_serial(serial);
    static_cast<WaylandSeat*>(data)->on_keyboard_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    static_cast<WaylandSeat*>(data)->on_keyboard_modifiers(depressed, latched, locked, group, serial);
}

static void keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay)
{
    static_cast<WaylandSeat*>(data)->on_keyboard_repeat_info(rate, delay);
}

static wl_keyboard_listener const s_keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

WaylandSeat::WaylandSeat(WaylandClient& client)
    : m_client(client)
{
}

void WaylandSeat::note_input_serial(u32 serial)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.last_serial = serial;
}

void WaylandSeat::note_pointer_enter_serial(u32 serial)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.last_serial = serial;
    m_state.pointer_enter_serial = serial;
}

u32 WaylandSeat::pointer_enter_serial() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.pointer_enter_serial;
}

u32 WaylandSeat::last_serial() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.last_serial;
}

wl_pointer* WaylandSeat::wayland_pointer() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.pointer;
}

wl_seat* WaylandSeat::wayland_seat() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.seat;
}

i32 WaylandSeat::pointer_window() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.pointer_window;
}

u32 WaylandSeat::pointer_buttons() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.pointer_buttons;
}

bool WaylandSeat::text_input_enabled() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_text_input.enabled;
}

void WaylandSeat::forget_window(i32 window_id, wl_surface* surface, wl_surface* menubar_surface)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.pointer_surface && (m_state.pointer_surface == surface || m_state.pointer_surface == menubar_surface))
        m_state.pointer_surface = nullptr;
    if (m_state.pointer_window == window_id)
        m_state.pointer_window = -1;
    if (m_state.focused_window == window_id)
        m_state.focused_window = -1;
}

void WaylandSeat::forget_popup(i32 popup_id)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.pointer_popup == popup_id)
        m_state.pointer_popup = -1;
}

void WaylandSeat::on_keyboard_repeat_info(i32 rate, i32 delay)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.repeat_rate = max(0, rate);
    m_state.repeat_delay_ms = max(0, delay);
    trace("LibWM/Wayland: repeat_info rate={}/s delay={}ms", m_state.repeat_rate, m_state.repeat_delay_ms);
    if (m_state.repeat_rate == 0)
        stop_key_repeat();
}

void WaylandSeat::start_key_repeat(u32 key)
{
    if (m_state.repeat_rate <= 0 || !m_state.xkb_keymap)
        return;
    if (!xkb_keymap_key_repeats(m_state.xkb_keymap, key + 8)) {
        // Modifiers and other non-repeating keys (the keymap decides).
        stop_key_repeat();
        return;
    }
    // A press for the key already repeating is another source's repeat: this
    // client generates one stream, and a rogue compositor may generate another.
    // Do not restart our delay on it.
    if (m_state.repeating_key == key && m_state.repeat_timer_running)
        return;

    if (!m_state.repeat_timer)
        m_state.repeat_timer = Core::Timer::create_single_shot(max(m_state.repeat_delay_ms, 1), [this] { repeat_tick(); });
    m_state.repeating_key = key;
    m_state.repeat_timer_running = true;
    m_state.repeat_timer->restart(max(m_state.repeat_delay_ms, 1));
}

void WaylandSeat::stop_key_repeat()
{
    m_state.repeating_key = 0;
    if (m_state.repeat_timer)
        m_state.repeat_timer->stop();
    m_state.repeat_timer_running = false;
}

void WaylandSeat::repeat_tick()
{
    MutexLocker locker(m_client.m_mutex);
    m_state.repeat_timer_running = false;
    if (m_state.repeating_key == 0 || !m_state.xkb_state || m_state.repeat_rate <= 0) {
        stop_key_repeat();
        return;
    }
    // Still the focused window's key: a focus change or capability loss stops
    // repeat from its event handler, but re-check in case that was missed.
    if (m_state.focused_window < 0) {
        stop_key_repeat();
        return;
    }
    deliver_key(m_state.repeating_key, true);
    m_state.repeat_timer_running = true;
    m_state.repeat_timer->restart(max(1000 / m_state.repeat_rate, 1));
}

void WaylandSeat::bind(wl_seat* seat)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.seat = seat;
    wl_seat_add_listener(seat, &s_seat_listener, this);
    m_client.m_data_device->maybe_create_data_device();
    ensure_text_input();
}

void WaylandSeat::set_text_input_manager(zwp_text_input_manager_v3* manager)
{
    MutexLocker locker(m_client.m_mutex);
    m_text_input.manager = manager;
    ensure_text_input();
}

void WaylandSeat::ensure_text_input()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_text_input.device || !m_text_input.manager || !m_state.seat)
        return;
    m_text_input.device = zwp_text_input_manager_v3_get_text_input(m_text_input.manager, m_state.seat);
    if (m_text_input.device)
        zwp_text_input_v3_add_listener(m_text_input.device, &s_text_input_listener, this);
}

void WaylandSeat::set_text_input_callbacks(TextInputCommit commit, TextInputPreedit preedit, TextInputDeleteSurrounding delete_surrounding)
{
    MutexLocker locker(m_client.m_mutex);
    m_text_input.on_commit = move(commit);
    m_text_input.on_preedit = move(preedit);
    m_text_input.on_delete_surrounding = move(delete_surrounding);
}

void WaylandSeat::set_text_input_enabled(bool enabled)
{
    MutexLocker locker(m_client.m_mutex);
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
    if (m_client.m_display)
        wl_display_flush(m_client.m_display);
}

void WaylandSeat::set_text_input_cursor_rect(Gfx::IntRect const& rect)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_text_input.cursor_rect == rect)
        return;
    m_text_input.cursor_rect = rect;
    if (!m_text_input.device || !m_text_input.enabled)
        return;
    zwp_text_input_v3_set_cursor_rectangle(m_text_input.device, rect.x(), rect.y(), rect.width(), rect.height());
    zwp_text_input_v3_commit(m_text_input.device);
    if (m_client.m_display)
        wl_display_flush(m_client.m_display);
}

void WaylandSeat::on_seat_capabilities(u32 capabilities)
{
    MutexLocker locker(m_client.m_mutex);
    // A capability that goes away takes the device object with it: the
    // compositor makes it inert, and it never delivers another event. Drop it, so
    // that the capability coming back (a keyboard plugged in again) gets a fresh
    // one, with its keymap and enter.
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && m_state.pointer) {
        // Observers quote the pointer they were created with, so they go with it.
        m_client.destroy_mouse_observers();
        wl_pointer_destroy(m_state.pointer);
        m_state.pointer = nullptr;
        m_state.pointer_popup = -1;
        m_state.pointer_window = -1;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && m_state.keyboard) {
        stop_key_repeat();
        wl_keyboard_destroy(m_state.keyboard);
        m_state.keyboard = nullptr;
        m_state.focused_window = -1;
    }

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !m_state.pointer) {
        m_state.pointer = wl_seat_get_pointer(m_state.seat);
        wl_pointer_add_listener(m_state.pointer, &s_pointer_listener, this);
        // A tracker may have been enabled before a pointer existed (or while the
        // pointer was away): create its observers now that there is one.
        m_client.create_mouse_observers();
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !m_state.keyboard) {
        m_state.keyboard = wl_seat_get_keyboard(m_state.seat);
        wl_keyboard_add_listener(m_state.keyboard, &s_keyboard_listener, this);
    }
    trace("LibWM/Wayland: seat capabilities 0x{:x} (pointer={}, keyboard={})", capabilities,
        m_state.pointer != nullptr, m_state.keyboard != nullptr);
}

void WaylandSeat::on_pointer_enter(wl_surface* surface, Gfx::IntPoint position)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.pointer_popup = m_client.popup_id_for_surface(surface);
    m_state.pointer_window = m_state.pointer_popup >= 0 ? -1 : m_client.window_id_for_surface(surface);
    m_state.pointer_surface = surface;
    m_state.pointer_position = position;
    trace("LibWM/Wayland: pointer entered {} {} at {},{}",
        m_state.pointer_popup >= 0 ? "popup" : "window",
        m_state.pointer_popup >= 0 ? m_state.pointer_popup : m_state.pointer_window,
        position.x(), position.y());

    if (m_state.pointer_popup >= 0) {
        // Popups (menus, context menus, comboboxes) use the default arrow. Without
        // this the cursor keeps the last style set by the window, so leaving a
        // text field for a menu would keep showing the I-beam.
        apply_standard_cursor_locked(Gfx::StandardCursor::Arrow);
        if (m_client.m_input.popup_motion)
            m_client.m_input.popup_motion(m_state.pointer_popup, position);
        return;
    }
    if (m_state.pointer_window >= 0) {
        if (auto const* window_surface = m_client.find(m_state.pointer_window))
            apply_window_cursor(*window_surface);
        if (m_client.m_input.window_entered)
            m_client.m_input.window_entered(m_state.pointer_window);
        // The window's hovered widget follows mouse motion, and the pointer only
        // sends motion once it is inside. Without this, entering a window straight
        // onto the location bar leaves the widget unaware until the pointer moves
        // again, so the I-beam (and hover highlight) arrives late.
        dispatch_pointer_motion(position);
    }
}

void WaylandSeat::on_pointer_leave()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.pointer_popup >= 0) {
        m_state.pointer_popup = -1;
        m_state.pointer_window = -1;
        return;
    }
    if (m_state.pointer_window >= 0 && m_client.m_input.menubar_left)
        m_client.m_input.menubar_left(m_state.pointer_window);
    if (m_state.pointer_window >= 0 && m_client.m_input.window_left)
        m_client.m_input.window_left(m_state.pointer_window);
    m_state.pointer_window = -1;
    m_state.pointer_surface = nullptr;
    m_state.axis_accum_x = 0;
    m_state.axis_accum_y = 0;
}

void WaylandSeat::on_pointer_motion(Gfx::IntPoint position)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.pointer_position = position;
    dispatch_pointer_motion(position);
}

void WaylandSeat::dispatch_pointer_motion(Gfx::IntPoint position)
{
    if (m_state.pointer_popup >= 0) {
        if (m_client.m_input.popup_motion)
            m_client.m_input.popup_motion(m_state.pointer_popup, position);
        return;
    }
    if (m_state.pointer_window < 0)
        return;

    auto* window_surface = m_client.find(m_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_state.pointer_surface == window_surface->menubar_surface) {
            if (m_client.m_input.menubar_motion)
                m_client.m_input.menubar_motion(m_state.pointer_window, position);
            return;
        }
        if (m_client.m_input.menubar_left)
            m_client.m_input.menubar_left(m_state.pointer_window);
        if (m_client.m_input.mouse_move)
            m_client.m_input.mouse_move(m_state.pointer_window, position, m_state.pointer_buttons, current_modifiers());
        return;
    }
    int const csd_h = window_surface ? window_surface->csd_height() : 0;
    int const inset = window_surface ? window_surface->inset : 0;
    if (csd_h > 0 && position.y() < csd_h) {
        if (m_client.m_input.menubar_left)
            m_client.m_input.menubar_left(m_state.pointer_window);
        return;
    }
    if (inset > 0 && position.y() >= csd_h && position.y() < csd_h + inset) {
        if (m_client.m_input.menubar_motion)
            m_client.m_input.menubar_motion(m_state.pointer_window, position.translated(0, -csd_h));
        return;
    }
    if (m_client.m_input.menubar_left)
        m_client.m_input.menubar_left(m_state.pointer_window);
    if (m_client.m_input.mouse_move)
        m_client.m_input.mouse_move(m_state.pointer_window, position.translated(0, -(csd_h + inset)), m_state.pointer_buttons, current_modifiers());
}

void WaylandSeat::on_pointer_button(u32 button, bool pressed)
{
    MutexLocker locker(m_client.m_mutex);
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

    if (m_state.pointer_popup >= 0) {
        if (m_client.m_input.popup_button)
            m_client.m_input.popup_button(m_state.pointer_popup, m_state.pointer_position, pressed);
        return;
    }
    if (m_state.pointer_window < 0)
        return;

    auto* window_surface = m_client.find(m_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_state.pointer_surface == window_surface->menubar_surface) {
            if (pressed && m_client.m_input.menubar_press)
                m_client.m_input.menubar_press(m_state.pointer_window, m_state.pointer_position);
            return;
        }
        auto position = m_state.pointer_position;
        if (pressed) {
            m_state.pointer_buttons |= serenity_button;
        } else {
            m_state.pointer_buttons &= ~serenity_button;
        }
        if (pressed) {
            if (m_client.m_input.mouse_down)
                m_client.m_input.mouse_down(m_state.pointer_window, position, serenity_button, m_state.pointer_buttons, current_modifiers());
        } else {
            if (m_client.m_input.mouse_up)
                m_client.m_input.mouse_up(m_state.pointer_window, position, serenity_button, m_state.pointer_buttons, current_modifiers());
        }
        return;
    }
    int const csd_h = window_surface ? window_surface->csd_height() : 0;
    int const inset = window_surface ? window_surface->inset : 0;
    if (csd_h > 0 && m_state.pointer_position.y() < csd_h) {
        if (pressed && window_surface) {
            auto const pos = m_state.pointer_position;
            int const w = window_surface->size.width();
            Gfx::IntRect const close_rect { w - 20, 4, 16, 14 };
            Gfx::IntRect const max_rect { w - 38, 4, 16, 14 };
            Gfx::IntRect const min_rect { w - (window_surface->resizable ? 56 : 38), 4, 16, 14 };
            if (button == BTN_LEFT) {
                if (close_rect.contains(pos)) {
                    trace("LibWM/Wayland: CSD close button clicked on window {}", m_state.pointer_window);
                    if (m_client.m_input.window_close_request)
                        m_client.m_input.window_close_request(m_state.pointer_window);
                } else if (window_surface->resizable && max_rect.contains(pos)) {
                    trace("LibWM/Wayland: CSD maximize button clicked on window {}", m_state.pointer_window);
                    m_client.set_maximized(m_state.pointer_window, !window_surface->maximized);
                } else if (min_rect.contains(pos)) {
                    trace("LibWM/Wayland: CSD minimize button clicked on window {}", m_state.pointer_window);
                    m_client.set_minimized(m_state.pointer_window);
                } else if (window_surface->toplevel && m_state.seat) {
                    trace("LibWM/Wayland: CSD titlebar drag move on window {}", m_state.pointer_window);
                    xdg_toplevel_move(window_surface->toplevel, m_state.seat, m_state.last_serial);
                    wl_display_flush(m_client.m_display);
                }
            } else if (button == BTN_RIGHT && window_surface->toplevel && m_state.seat) {
                xdg_toplevel_show_window_menu(window_surface->toplevel, m_state.seat, m_state.last_serial, pos.x(), pos.y());
                wl_display_flush(m_client.m_display);
            }
        }
        return;
    }
    if (inset > 0 && m_state.pointer_position.y() >= csd_h && m_state.pointer_position.y() < csd_h + inset) {
        if (pressed && m_client.m_input.menubar_press)
            m_client.m_input.menubar_press(m_state.pointer_window, m_state.pointer_position.translated(0, -csd_h));
        return;
    }

    auto position = m_state.pointer_position.translated(0, -(csd_h + inset));
    if (pressed) {
        m_state.pointer_buttons |= serenity_button;
        trace("LibWM/Wayland: mouse down window {} at {},{} button={}", m_state.pointer_window, position.x(), position.y(), serenity_button);
        if (m_client.m_input.mouse_down)
            m_client.m_input.mouse_down(m_state.pointer_window, position, serenity_button, m_state.pointer_buttons, current_modifiers());
    } else {
        m_state.pointer_buttons &= ~serenity_button;
        trace("LibWM/Wayland: mouse up window {} at {},{} button={}", m_state.pointer_window, position.x(), position.y(), serenity_button);
        if (m_client.m_input.mouse_up)
            m_client.m_input.mouse_up(m_state.pointer_window, position, serenity_button, m_state.pointer_buttons, current_modifiers());
    }
}

void WaylandSeat::on_pointer_axis(i32 x, i32 y)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.pointer_popup >= 0) {
        if (y != 0 && m_client.m_input.popup_wheel)
            m_client.m_input.popup_wheel(m_state.pointer_popup, y);
        return;
    }
    if (m_state.pointer_window < 0) {
        trace("LibWM/Wayland: wheel with no window under the pointer");
        return;
    }
    auto* window_surface = m_client.find(m_state.pointer_window);
    if (window_surface && window_surface->menubar_surface) {
        if (m_state.pointer_surface == window_surface->menubar_surface)
            return;
        if (m_client.m_input.mouse_wheel)
            m_client.m_input.mouse_wheel(m_state.pointer_window, m_state.pointer_position, m_state.pointer_buttons, current_modifiers(), x, y);
        return;
    }
    int inset = window_surface ? window_surface->total_top_inset() : 0;
    trace("LibWM/Wayland: wheel on window {} at {},{} delta {}x{}", m_state.pointer_window,
        m_state.pointer_position.x(), m_state.pointer_position.y() - inset, x, y);
    if (m_client.m_input.mouse_wheel)
        m_client.m_input.mouse_wheel(m_state.pointer_window, m_state.pointer_position.translated(0, -inset), m_state.pointer_buttons, current_modifiers(), x, y);
}

void WaylandSeat::on_pointer_axis_delta(double x, double y)
{
    MutexLocker locker(m_client.m_mutex);
    // Wayland delivers smooth axis values that are often fractional (trackpads
    // and high-resolution wheels). SerenityOS widgets expect whole scroll steps,
    // so accumulate and emit the integral part, carrying the remainder.
    m_state.axis_accum_x += x;
    m_state.axis_accum_y += y;
    auto step_x = static_cast<i32>(m_state.axis_accum_x);
    auto step_y = static_cast<i32>(m_state.axis_accum_y);
    if (Environment::is_enabled("LIBWM_AXIS_DEBUG"))
        dbgln("LibWM/Wayland: axis delta {}x{} -> step {}x{}", x, y, step_x, step_y);
    if (step_x == 0 && step_y == 0)
        return;
    m_state.axis_accum_x -= step_x;
    m_state.axis_accum_y -= step_y;
    on_pointer_axis(step_x, step_y);
}

void WaylandSeat::on_keyboard_keymap(int fd, u32 size)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_state.xkb_context)
        m_state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    auto* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    close(fd);

    if (m_state.xkb_keymap)
        xkb_keymap_unref(m_state.xkb_keymap);
    m_state.xkb_keymap = xkb_keymap_new_from_string(m_state.xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);

    if (m_state.xkb_state)
        xkb_state_unref(m_state.xkb_state);
    m_state.xkb_state = m_state.xkb_keymap ? xkb_state_new(m_state.xkb_keymap) : nullptr;

    trace("LibWM/Wayland: received keymap ({} bytes), state={}", size, m_state.xkb_state != nullptr);
}

u32 WaylandSeat::current_modifiers() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.modifiers;
}

void WaylandSeat::on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32)
{
    MutexLocker locker(m_client.m_mutex);
    // The compositor's modifier state is authoritative. Key events alone are not
    // enough: if a release is lost because focus moved while Ctrl/Alt was held,
    // the next click and wheel event would silently keep that modifier forever.
    if (!m_state.xkb_state) {
        trace("LibWM/Wayland: modifiers depressed={:#x} locked={:#x} group={}", depressed, locked, group);
        return;
    }

    xkb_state_update_mask(m_state.xkb_state, depressed, latched, locked, 0, 0, group);

    u32 modifiers = 0;
    if (xkb_state_mod_name_is_active(m_state.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Shift;
    if (xkb_state_mod_name_is_active(m_state.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Ctrl;
    if (xkb_state_mod_name_is_active(m_state.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Alt;
    if (xkb_state_mod_name_is_active(m_state.xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= Mod_Super;
    m_state.modifiers = modifiers;

    trace("LibWM/Wayland: modifiers depressed={:#x} locked={:#x} group={} -> {:#x}", depressed, locked, group, m_state.modifiers);
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

void WaylandSeat::deliver_key(u32 key, bool pressed)
{
    if (!m_state.xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    u32 code_point = xkb_state_key_get_utf32(m_state.xkb_state, code);
    u32 key_code = static_cast<u32>(serenity_key_code_from_evdev(key));

    // Serenity's WindowServer turned Ctrl+<letter> into the control character
    // itself, so a client sees Ctrl-C as code point 3 rather than 'c' with a ctrl
    // flag; LibVT relies on that, and so does every terminal program that expects a
    // SIGINT.
    if ((current_modifiers() & Mod_Ctrl) && code_point >= '@' && code_point < 0x7f)
        code_point &= 0x1f;

    if (m_client.m_input.key)
        m_client.m_input.key(m_state.focused_window, code_point, key_code, 0, current_modifiers(), key, pressed);
}

void WaylandSeat::on_keyboard_key(u32 key, bool pressed)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_state.xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    // Update the xkb state from the key itself so code_point reflects
    // Shift/CapsLock/etc. even without modifier events.
    xkb_state_update_key(m_state.xkb_state, code, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    if (u32 bit = modifier_bit_for_evdev(key); bit != 0) {
        if (pressed)
            m_state.modifiers |= bit;
        else
            m_state.modifiers &= ~bit;
    }

    deliver_key(key, pressed);

    if (pressed)
        start_key_repeat(key);
    else if (m_state.repeating_key == key)
        stop_key_repeat();
}

void WaylandSeat::on_keyboard_focus(wl_surface* surface, bool entered)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.focused_window = entered ? m_client.window_id_for_surface(surface) : -1;
    stop_key_repeat();
}

Gfx::IntPoint WaylandSeat::global_pointer_position()
{
    MutexLocker locker(m_client.m_mutex);
    // Popups (server-rendered menus) are layer surfaces with a known position.
    if (m_state.pointer_popup >= 0) {
        if (auto* popup = m_client.find_popup(m_state.pointer_popup))
            return popup->layer_position.translated(m_state.pointer_position);
    }
    if (m_state.pointer_window >= 0) {
        if (auto* window_surface = m_client.find(m_state.pointer_window)) {
            // Layer surfaces (Taskbar, Desktop) know their output position; for
            // toplevels it comes from the compositor's position event.
            auto origin = window_surface->is_layer ? window_surface->layer_output_position
                                                   : window_surface->global_position;
            return origin.translated(m_state.pointer_position);
        }
    }
    return m_state.pointer_position;
}

static StringView standard_cursor_filename(Gfx::StandardCursor cursor)
{
    switch (cursor) {
    case Gfx::StandardCursor::Arrow:
        return "arrow.x2y2.png"sv;
    case Gfx::StandardCursor::Crosshair:
        return "crosshair.png"sv;
    case Gfx::StandardCursor::IBeam:
        return "i-beam.png"sv;
    case Gfx::StandardCursor::ResizeHorizontal:
        return "resize-horizontal.png"sv;
    case Gfx::StandardCursor::ResizeVertical:
        return "resize-vertical.png"sv;
    case Gfx::StandardCursor::ResizeDiagonalTLBR:
        return "resize-diagonal-tlbr.png"sv;
    case Gfx::StandardCursor::ResizeDiagonalBLTR:
        return "resize-diagonal-bltr.png"sv;
    case Gfx::StandardCursor::ResizeColumn:
        return "resize-column.png"sv;
    case Gfx::StandardCursor::ResizeRow:
        return "resize-row.png"sv;
    case Gfx::StandardCursor::Hand:
        return "hand.x8y4.png"sv;
    case Gfx::StandardCursor::Help:
        return "help.x1y1.png"sv;
    case Gfx::StandardCursor::Drag:
        return "drag.png"sv;
    case Gfx::StandardCursor::DragCopy:
        return "drag-copy.x7y7.png"sv;
    case Gfx::StandardCursor::Move:
        return "move.png"sv;
    case Gfx::StandardCursor::Wait:
        return "wait.f14t100.png"sv;
    case Gfx::StandardCursor::Disallowed:
        return "disallowed.png"sv;
    case Gfx::StandardCursor::Eyedropper:
        return "eyedropper.x2y2.png"sv;
    case Gfx::StandardCursor::Zoom:
        return "zoom.x0y0.png"sv;
    default:
        return {};
    }
}

Optional<WaylandSeat::CachedCursor> WaylandSeat::load_standard_cursor(Gfx::StandardCursor cursor)
{
    auto filename = standard_cursor_filename(cursor);
    if (filename.is_empty())
        return {};

    auto theme = m_client.m_serenity_shell->host_setting("cursor.theme"sv, "Default"sv);
    auto cache_key = ByteString::formatted("{}/{}", theme, filename);
    if (auto cached = m_state.cursor_cache.get(cache_key); cached.has_value())
        return cached.value();

    auto res_root = Environment::resource_root();
    auto loaded = Gfx::Bitmap::load_from_file(ByteString::formatted("{}/cursor-themes/{}/{}", res_root, theme, filename));
    if (loaded.is_error() && theme != "Default"sv)
        loaded = Gfx::Bitmap::load_from_file(ByteString::formatted("{}/cursor-themes/Default/{}", res_root, filename));
    if (loaded.is_error())
        return {};

    auto bitmap = loaded.release_value();
    auto params = Gfx::CursorParams::parse_from_filename(filename, bitmap->rect().center()).constrained(*bitmap);
    // Animated cursors are horizontal sprite sheets; present the first frame.
    if (params.frames() > 1 && bitmap->width() >= static_cast<int>(params.frames())) {
        int const frame_width = bitmap->width() / static_cast<int>(params.frames());
        if (auto cropped = bitmap->cropped({ 0, 0, frame_width, bitmap->height() }); !cropped.is_error())
            bitmap = cropped.release_value();
        params = Gfx::CursorParams::parse_from_filename(filename, bitmap->rect().center()).constrained(*bitmap);
    }

    CachedCursor cursor_entry { move(bitmap), params.hotspot() };
    m_state.cursor_cache.set(cache_key, cursor_entry);
    return cursor_entry;
}

void WaylandSeat::apply_window_cursor(WaylandClient::WindowSurface const& window_surface)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_state.pointer || !m_client.m_compositor || !m_client.m_shm)
        return;

    auto const requested_cursor = static_cast<Gfx::StandardCursor>(window_surface.cursor_type);
    // Serenity's WindowServer treats "no override" as the default arrow. Keeping
    // the previous cursor here made an I-beam persist after leaving a text field
    // for a button, the bookmark bar, or another widget without a cursor.
    auto const cursor_enum = requested_cursor == Gfx::StandardCursor::None ? Gfx::StandardCursor::Arrow : requested_cursor;
    bool const already_applied_for_this_enter = m_state.applied_cursor_serial == m_state.pointer_enter_serial;

    if (!window_surface.custom_cursor && cursor_enum == Gfx::StandardCursor::Hidden) {
        if (already_applied_for_this_enter && m_state.applied_cursor_hidden)
            return;
        wl_pointer_set_cursor(m_state.pointer, m_state.pointer_enter_serial, nullptr, 0, 0);
        wl_display_flush(m_client.m_display);
        m_state.applied_cursor = nullptr;
        m_state.applied_cursor_hidden = true;
        m_state.applied_cursor_serial = m_state.pointer_enter_serial;
        trace("LibWM/Wayland: hid cursor on window {}", window_surface.window_id);
        return;
    }

    CachedCursor cursor_entry;
    if (window_surface.custom_cursor) {
        cursor_entry = { window_surface.custom_cursor, window_surface.custom_cursor->rect().center() };
    } else if (cursor_enum != Gfx::StandardCursor::None) {
        auto loaded = load_standard_cursor(cursor_enum);
        if (!loaded.has_value())
            return;
        cursor_entry = loaded.release_value();
    } else {
        return;
    }

    apply_cursor_bitmap(cursor_entry.bitmap, cursor_entry.hotspot);
}

void WaylandSeat::preload_cursors()
{
    // A cursor is decoded from the resource tree the first time a widget asks for
    // it. On a cold start that load lands on the UI thread exactly when the user
    // moves onto the location bar (I-beam) or a link (hand), which feels like a
    // stutter. Warm the common ones once the host settings name the theme.
    auto const start = MonotonicTime::now();
    size_t loaded = 0;
    for (auto cursor : { Gfx::StandardCursor::Arrow, Gfx::StandardCursor::IBeam, Gfx::StandardCursor::Hand, Gfx::StandardCursor::Crosshair }) {
        if (load_standard_cursor(cursor).has_value())
            ++loaded;
    }
    trace("LibWM/Wayland: preloaded {} cursors in {} ms", loaded, (MonotonicTime::now() - start).to_milliseconds());
}

void WaylandSeat::apply_standard_cursor(Gfx::StandardCursor cursor)
{
    MutexLocker locker(m_client.m_mutex);
    apply_standard_cursor_locked(cursor);
}

void WaylandSeat::apply_standard_cursor_locked(Gfx::StandardCursor cursor)
{
    if (!m_state.pointer || !m_client.m_compositor || !m_client.m_shm)
        return;
    auto loaded = load_standard_cursor(cursor);
    if (!loaded.has_value())
        return;
    apply_cursor_bitmap(loaded->bitmap, loaded->hotspot);
}

void WaylandSeat::apply_cursor_bitmap(RefPtr<Gfx::Bitmap> const& bitmap, Gfx::IntPoint hotspot)
{
    bool const already_applied_for_this_enter = m_state.applied_cursor_serial == m_state.pointer_enter_serial;

    if (!bitmap || bitmap->size().is_empty())
        return;

    // LibGUI re-sends the cursor as the pointer crosses widgets; don't re-upload
    // the one that is already showing.
    if (already_applied_for_this_enter && !m_state.applied_cursor_hidden && m_state.applied_cursor == bitmap)
        return;

    if (!m_state.cursor_surface)
        m_state.cursor_surface = wl_compositor_create_surface(m_client.m_compositor);
    if (!m_state.cursor_surface)
        return;

    m_client.purge_released_buffers(m_state.cursor_buffers);

    // A fresh buffer per change: the compositor may still be reading the previous
    // one, and cursor changes are rare enough that reuse is not worth tracking.
    auto buffer = Core::AnonymousBuffer::create_with_size(Gfx::Bitmap::minimum_pitch(bitmap->width(), Gfx::BitmapFormat::BGRA8888) * static_cast<size_t>(bitmap->height()));
    if (buffer.is_error())
        return;
    auto wrapped = Gfx::Bitmap::create_with_anonymous_buffer(Gfx::BitmapFormat::BGRA8888, buffer.value(), bitmap->size(), 1);
    if (wrapped.is_error())
        return;
    auto& upload = *wrapped.value();
    size_t const row_bytes = static_cast<size_t>(bitmap->width()) * sizeof(u32);
    for (int y = 0; y < bitmap->height(); ++y)
        memcpy(upload.scanline(y), bitmap->scanline(y), row_bytes);

    m_client.bind_bitmap(m_state.cursor_surface, buffer.value(), upload, m_state.cursor_buffers);
    wl_surface_commit(m_state.cursor_surface);
    wl_pointer_set_cursor(m_state.pointer, m_state.pointer_enter_serial, m_state.cursor_surface, hotspot.x(), hotspot.y());
    wl_display_flush(m_client.m_display);

    m_state.applied_cursor = bitmap;
    m_state.applied_cursor_hidden = false;
    m_state.applied_cursor_serial = m_state.pointer_enter_serial;
    trace("LibWM/Wayland: set cursor ({}x{}, hotspot {},{})", bitmap->width(), bitmap->height(), hotspot.x(), hotspot.y());
}

}
