/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>

struct wl_buffer;
struct wl_compositor;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_display;
struct wl_keyboard;
struct wl_output;
struct wl_pointer;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct wl_surface;
struct xdg_popup;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace Core {
class Notifier;
}

namespace LibWM {

// A thin wrapper over the real Wayland compositor.
//
// This is the source of truth for environmental state (principle P1): output
// logical size and scale, seat capabilities, input, shm, and the xdg-shell
// objects used to present LibGUI windows. It is driven from the WindowServer
// thread's event loop.
class WaylandClient {
public:
    // Input is delivered through these callbacks, which the WindowServer
    // connection installs to translate into Serenity WindowClient events.
    struct InputCallbacks {
        Function<void(i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers)> mouse_move;
        Function<void(i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers)> mouse_down;
        Function<void(i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers)> mouse_up;
        Function<void(i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, i32 wheel_delta_x, i32 wheel_delta_y)> mouse_wheel;
        Function<void(i32 window_id, u32 code_point, u32 key, u8 map_entry_index, u32 modifiers, u32 scancode, bool is_press)> key;
        Function<void(i32 window_id)> window_entered;
        Function<void(i32 window_id)> window_left;
        Function<void(i32 window_id)> window_close_request;
        Function<void(i32 window_id, bool activated)> window_activation;
        Function<void(i32 window_id, Gfx::IntSize content_size)> window_resize;
        // Menubar (top inset) and popup (menu) input.
        Function<void(i32 window_id, Gfx::IntPoint position)> menubar_motion;
        Function<void(i32 window_id)> menubar_left;
        Function<void(i32 window_id, Gfx::IntPoint position)> menubar_press;
        Function<void(i32 popup_id, Gfx::IntPoint position)> popup_motion;
        Function<void(i32 popup_id, Gfx::IntPoint position, bool pressed)> popup_button;
        Function<void(i32 popup_id)> popup_closed;
    };

    static WaylandClient& the();

    ErrorOr<void> ensure_connected();
    bool is_connected() const { return m_display != nullptr; }

    // Logical screen size and output scale as reported by the compositor.
    Gfx::IntSize screen_size() const { return m_screen_size; }
    int output_scale() const { return m_scale; }

    void set_input_callbacks(InputCallbacks callbacks) { m_input = move(callbacks); }

    // --- Clipboard (native Wayland data device) ---
    // Called when the compositor's selection changes; the mime type is a
    // preferred Serenity-side type (e.g. "text/plain").
    void set_clipboard_changed_callback(Function<void(ByteString const&)> callback) { m_clipboard_changed = move(callback); }
    // Fetch the current selection, preferring text then files/images. Blocks until the source closes the pipe.
    ErrorOr<ByteBuffer> read_clipboard(ByteString& out_mime_type);
    // Advertise a set of mime-type -> bytes representations as the selection.
    void write_clipboard(HashMap<ByteString, ByteBuffer> offers);

    // Window <-> xdg_toplevel lifecycle.
    void create_window(i32 window_id, Gfx::IntSize, ByteString const& title, bool has_alpha, bool resizable);
    void destroy_window(i32 window_id);
    void set_title(i32 window_id, ByteString const& title);

    // Window state (maps to xdg_toplevel).
    void set_fullscreen(i32 window_id, bool fullscreen);
    void set_maximized(i32 window_id, bool maximized);
    void set_minimized(i32 window_id);

    // Attach the client's shared bitmap (given as an fd) and commit. `size` is
    // the backing store size; `visible_size` is the part to present (they differ
    // during interactive resize, when LibGUI over-allocates by a margin).
    void attach_and_commit(i32 window_id, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha);

    // Reserve a strip at the top of the window (a menubar) drawn by `draw`,
    // with the client's content shifted down by `inset` pixels.
    void set_window_inset(i32 window_id, int inset, Function<void(Gfx::Bitmap&, Gfx::IntRect)> draw);

    // Popup surfaces for server-rendered menus.
    void create_popup(i32 popup_id, i32 parent_window_id, Gfx::IntRect anchor, Gfx::IntSize size);
    void present_popup(i32 popup_id, Gfx::Bitmap const& bitmap);
    void destroy_popup(i32 popup_id);

    // Wayland event dispatch (registered on the owning event loop).
    void dispatch();

    // --- called by the C protocol listeners (see WaylandClient.cpp) ---
    struct BufferRecord {
        wl_buffer* buffer { nullptr };
        bool released { false };
    };
    void set_compositor(wl_compositor* compositor) { m_compositor = compositor; }
    void set_shm(wl_shm* shm) { m_shm = shm; }
    void add_seat(wl_seat* seat);
    void add_wm_base(xdg_wm_base* wm_base);
    void set_decoration_manager(zxdg_decoration_manager_v1* manager) { m_decoration_manager = manager; }
    void set_data_device_manager(wl_data_device_manager* manager);
    void add_output(wl_output* output);
    void set_xdg_output_manager(zxdg_output_manager_v1* manager);
    void on_output_logical_size(wl_output*, Gfx::IntSize logical_size);
    void on_output_mode(Gfx::IntSize physical_size) { m_physical_size = physical_size; }
    void on_output_scale(int factor) { m_scale = factor > 0 ? factor : 1; }
    void on_output_done();

    void on_seat_capabilities(u32 capabilities);
    i32 window_id_for_surface(wl_surface*) const;
    void on_pointer_enter(wl_surface*, Gfx::IntPoint);
    void on_pointer_leave();
    void on_pointer_motion(Gfx::IntPoint);
    void on_pointer_button(u32 button, bool pressed);
    void on_pointer_axis(i32 x, i32 y);
    void on_keyboard_keymap(int fd, u32 size);
    void on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32 serial);
    void on_keyboard_key(u32 key, bool pressed);
    void on_keyboard_focus(wl_surface*, bool entered);
    void on_toplevel_configure(xdg_toplevel*, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized);
    void on_toplevel_close(xdg_toplevel*);

    void on_data_offer(wl_data_offer*);
    void on_data_offer_mime(wl_data_offer*, char const* mime_type);
    void on_selection(wl_data_offer*);
    void on_source_send(wl_data_source*, char const* mime_type, int fd);
    void on_source_cancelled(wl_data_source*);
    void on_popup_done(xdg_popup*);
    void on_surface_configured(xdg_surface*);
    void note_input_serial(u32 serial) { m_last_input_serial = serial; }

private:
    WaylandClient() = default;

    struct WindowSurface {
        i32 window_id { -1 };
        wl_surface* surface { nullptr };
        xdg_surface* xdg_surface_object { nullptr };
        xdg_toplevel* toplevel { nullptr };
        zxdg_toplevel_decoration_v1* decoration { nullptr };
        ByteString title;
        Gfx::IntSize size;
        bool has_alpha { false };
        bool resizable { true };
        bool fullscreen { false };
        Gfx::IntSize fixed_size;
        Vector<NonnullOwnPtr<BufferRecord>> buffers;
        // Optional top strip (menubar) composited above the client content.
        int inset { 0 };
        Function<void(Gfx::Bitmap&, Gfx::IntRect)> draw_inset;
        Core::AnonymousBuffer composed_buffer;
        RefPtr<Gfx::Bitmap> composed_bitmap;
    };

    struct Popup {
        i32 id { -1 };
        wl_surface* surface { nullptr };
        xdg_surface* xdg_surface_object { nullptr };
        xdg_popup* popup { nullptr };
        bool configured { false };
        Core::AnonymousBuffer buffer;
        RefPtr<Gfx::Bitmap> bitmap;
        RefPtr<Gfx::Bitmap> pending;
        Vector<NonnullOwnPtr<BufferRecord>> buffers;
    };

    WindowSurface* find(i32 window_id);
    Popup* find_popup(i32 popup_id);
    i32 popup_id_for_surface(wl_surface*) const;
    void purge_released_buffers(WindowSurface&);
    void purge_released_buffers(Popup&);
    void bind_bitmap(wl_surface*, Core::AnonymousBuffer const&, Gfx::Bitmap const&, Vector<NonnullOwnPtr<BufferRecord>>&);
    u32 current_modifiers() const;
    void maybe_create_data_device();
    ByteString preferred_mime_for(wl_data_offer*) const;

    wl_display* m_display { nullptr };
    wl_registry* m_registry { nullptr };
    wl_compositor* m_compositor { nullptr };
    wl_shm* m_shm { nullptr };
    wl_seat* m_seat { nullptr };
    wl_pointer* m_pointer { nullptr };
    wl_keyboard* m_keyboard { nullptr };
    wl_output* m_output { nullptr };
    Vector<wl_output*> m_outputs;
    zxdg_output_manager_v1* m_xdg_output_manager { nullptr };
    xdg_wm_base* m_wm_base { nullptr };
    zxdg_decoration_manager_v1* m_decoration_manager { nullptr };
    RefPtr<Core::Notifier> m_notifier;

    xkb_context* m_xkb_context { nullptr };
    xkb_keymap* m_xkb_keymap { nullptr };
    xkb_state* m_xkb_state { nullptr };

    wl_data_device_manager* m_data_device_manager { nullptr };
    wl_data_device* m_data_device { nullptr };
    wl_data_offer* m_current_offer { nullptr };
    Vector<wl_data_offer*> m_pending_offers;
    HashMap<u64, Vector<ByteString>> m_offer_mime_types;
    wl_data_source* m_data_source { nullptr };
    HashMap<ByteString, ByteBuffer> m_clipboard_offers;
    u32 m_last_input_serial { 0 };
    u32 m_modifiers { 0 };
    Function<void(ByteString const&)> m_clipboard_changed;

    InputCallbacks m_input;
    i32 m_pointer_window { -1 };
    i32 m_focused_window { -1 };
    Gfx::IntPoint m_pointer_position;
    u32 m_pointer_buttons { 0 };

    Gfx::IntSize m_physical_size;
    Gfx::IntSize m_screen_size;
    int m_scale { 1 };
    bool m_have_logical_size { false };

    HashMap<i32, NonnullOwnPtr<WindowSurface>> m_windows;
    HashMap<i32, NonnullOwnPtr<Popup>> m_popups;
    i32 m_pointer_popup { -1 };
};

}
