/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WaylandClient.h"
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>
#include <LibCore/Timer.h>
#include <LibGfx/Rect.h>

struct wl_keyboard;
struct wl_pointer;
struct wl_touch;
struct wl_pointer;
struct wl_seat;
struct wl_surface;
struct zwp_text_input_manager_v3;
struct zwp_text_input_v3;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace Gfx {
enum class StandardCursor;
}

namespace LibWM {

// wl_seat: pointer, keyboard (XKB), text input (zwp_text_input_v3) and the
// pointer cursor.
//
// Pointer and key events are routed to the right window, popup, menubar or CSD
// titlebar here, translated into SerenityOS terms (buttons, key codes, code
// points, modifiers), and delivered through WaylandClient's InputCallbacks.
// Shares WaylandClient's mutex.
class WaylandSeat {
public:
    explicit WaylandSeat(WaylandClient&);

    // The pointer's position in output coordinates, reconstructed from the
    // surface under the pointer and that surface's known origin. The compositor
    // owns window placement (and never reports the cursor directly), so this is
    // how the client places tooltips near the cursor.
    Gfx::IntPoint global_pointer_position();

    // --- text input / IME (zwp_text_input_v3) ---
    // The compositor relays preedit and committed text from the input method to
    // the focused client. The app enables text input only while a text field has
    // focus, and supplies the caret rectangle so candidate windows can follow it.
    using TextInputCommit = Function<void(String const&)>;
    using TextInputPreedit = Function<void(String const&, i32 cursor_begin, i32 cursor_end)>;
    using TextInputDeleteSurrounding = Function<void(i32 before_length, i32 after_length)>;
    void set_text_input_callbacks(TextInputCommit, TextInputPreedit, TextInputDeleteSurrounding);
    void set_text_input_enabled(bool);
    bool text_input_enabled() const;
    void set_text_input_cursor_rect(Gfx::IntRect const&);

    // --- for the other Wayland components ---
    wl_seat* wayland_seat() const;
    wl_pointer* wayland_pointer() const;
    // The serial of the most recent input event: required by requests that need
    // user intent (set_selection, start_drag, popup grabs, interactive move).
    u32 last_serial() const;
    // The serial of the last wl_pointer.enter: wp_pointer_warp_v1 validates it.
    u32 pointer_enter_serial() const;
    i32 pointer_window() const;
    u32 pointer_buttons() const;
    u32 current_modifiers() const;
    // Show the cursor `window_surface` asks for, if the pointer is over it.
    void apply_window_cursor(WaylandClient::WindowSurface const&);
    // Decode the cursors a window is likely to ask for, so the first move onto
    // a text field or link does not pay for the bitmap load.
    void preload_cursors();
    // Drop references to a window or popup that is being destroyed.
    void forget_window(i32 window_id, wl_surface* surface, wl_surface* menubar_surface);
    void forget_popup(i32 popup_id);

    // --- registry and protocol events ---
    void bind(wl_seat*);
    void set_text_input_manager(zwp_text_input_manager_v3*);
    void note_input_serial(u32 serial);
    void note_pointer_enter_serial(u32 serial);
    void on_seat_capabilities(u32 capabilities);
    void on_pointer_enter(wl_surface*, Gfx::IntPoint);
    // Deliver a motion position to the focused window/popup. Caller holds the client mutex.
    void dispatch_pointer_motion(Gfx::IntPoint);
    void on_pointer_leave();
    void on_pointer_motion(Gfx::IntPoint);
    void on_pointer_button(u32 button, bool pressed);
    void on_pointer_axis(i32 x, i32 y);
    void on_pointer_axis_delta(double x, double y);
    // Touch is translated to the primary pointer: LibGUI has no touch events,
    // so the first finger behaves like a mouse press/drag/release.
    void on_touch_down(wl_surface*, u32 id, Gfx::IntPoint position);
    void on_touch_motion(u32 id, Gfx::IntPoint position);
    void on_touch_up(u32 id);
    void on_touch_cancel();
    void on_keyboard_keymap(int fd, u32 size);
    void on_keyboard_repeat_info(i32 rate, i32 delay);
    void on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32 serial);
    void on_keyboard_key(u32 key, bool pressed);
    void on_keyboard_focus(wl_surface*, bool entered);
    void on_text_input_enter();
    void on_text_input_commit(String);
    void on_text_input_preedit(String, i32 cursor_begin, i32 cursor_end);
    void on_text_input_delete_surrounding(i32 before_length, i32 after_length);

private:
    struct CachedCursor {
        RefPtr<Gfx::Bitmap> bitmap;
        Gfx::IntPoint hotspot;
    };

    void ensure_text_input();
    Optional<CachedCursor> load_standard_cursor(Gfx::StandardCursor);
    // Send one key event from the current XKB state (no state change).
    void deliver_key(u32 key, bool pressed);
    void start_key_repeat(u32 key);
    void stop_key_repeat();
    void repeat_tick();
    void apply_cursor_bitmap(RefPtr<Gfx::Bitmap> const&, Gfx::IntPoint hotspot);
    void apply_standard_cursor(Gfx::StandardCursor);
    // Caller holds WaylandClient::the().mutex().
    void apply_standard_cursor_locked(Gfx::StandardCursor);

    struct State {
        wl_seat* seat { nullptr };
        wl_pointer* pointer { nullptr };
        wl_keyboard* keyboard { nullptr };
        wl_touch* touch { nullptr };
        bool touch_active { false };
        i32 touch_id { -1 };
        i32 touch_window { -1 };
        ::xkb_context* xkb_context { nullptr };
        ::xkb_keymap* xkb_keymap { nullptr };
        ::xkb_state* xkb_state { nullptr };
        u32 last_serial { 0 };
        u32 pointer_enter_serial { 0 };
        u32 modifiers { 0 };
        i32 pointer_window { -1 };
        i32 pointer_popup { -1 };
        wl_surface* pointer_surface { nullptr };
        wl_surface* cursor_surface { nullptr };
        Vector<NonnullOwnPtr<WaylandClient::BufferRecord>> cursor_buffers;
        // Decoded theme cursors, keyed by "<theme>/<file>", and what is showing
        // now (so repeated set_cursor calls for the same image are skipped).
        HashMap<ByteString, CachedCursor> cursor_cache;
        RefPtr<Gfx::Bitmap> applied_cursor;
        bool applied_cursor_hidden { false };
        u32 applied_cursor_serial { 0 };
        i32 focused_window { -1 };
        Gfx::IntPoint pointer_position;
        u32 pointer_buttons { 0 };
        // Wayland axis deltas are often fractional (trackpads, high-resolution
        // wheels); accumulate them so whole scroll steps reach the GUI.
        double axis_accum_x { 0 };
        double axis_accum_y { 0 };
        // wl_keyboard.repeat_info: 0 disables repeat.
        int repeat_rate { 0 };
        int repeat_delay_ms { 0 };
        u32 repeating_key { 0 };
        bool repeat_timer_running { false };
        RefPtr<Core::Timer> repeat_timer;
    };

    struct TextInputState {
        zwp_text_input_manager_v3* manager { nullptr };
        zwp_text_input_v3* device { nullptr };
        bool enabled { false };
        Gfx::IntRect cursor_rect;
        TextInputCommit on_commit;
        TextInputPreedit on_preedit;
        TextInputDeleteSurrounding on_delete_surrounding;
    };

    WaylandClient& m_client;
    State m_state;
    TextInputState m_text_input;
};

}
