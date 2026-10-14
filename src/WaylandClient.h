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
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>

struct wl_buffer;
struct wl_callback;
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
struct wp_viewporter;
struct wp_viewport;
struct xdg_popup;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct serenity_window_manager;
struct serenity_toplevel;

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
        // Drag-and-drop. The source end reports its outcome; the target end
        // receives the drop/motion with the transferred mime data.
        Function<void(bool accepted)> drag_finished;
        Function<void(i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, HashMap<String, ByteBuffer>)> drag_moved;
        Function<void(i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, HashMap<String, ByteBuffer>)> drag_dropped;
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
    // The backing-store scale the shim told the client to render at (usually the
    // output scale; 1 when client scaling is disabled). Distinct from output_scale
    // so present() maps buffers correctly regardless of a resize margin.
    int client_scale() const { return m_client_scale; }
    void set_client_scale(int scale) { m_client_scale = scale < 1 ? 1 : scale; }

    // The pointer's position in output coordinates, reconstructed from the
    // surface under the pointer and that surface's known origin. The compositor
    // owns window placement (and never reports the cursor directly), so this is
    // how the client places tooltips near the cursor.
    Gfx::IntPoint global_pointer_position();

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
    void create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize, ByteString const& title, bool has_alpha, bool resizable, i32 window_type, i32 window_mode, i32 parent_window_id, Gfx::IntRect const& launch_origin = {});
    void destroy_window(i32 window_id);
    void set_title(i32 window_id, ByteString const& title);

    // Whether the compositor currently wants the window's menu bar shown.
    bool window_menubar_visible(i32 window_id) const;

    // Update the size of a layer-surface window (the panel height, say). The
    // compositor controls the position of layer surfaces; for ordinary xdg
    // toplevels this is a no-op (their size is driven by a configure).
    void set_window_rect(i32 window_id, Gfx::IntSize size);

    // Window state (maps to xdg_toplevel).
    void set_fullscreen(i32 window_id, bool fullscreen);
    void set_maximized(i32 window_id, bool maximized);
    void set_minimized(i32 window_id);

    // The window's icon, pushed to the compositor over serenity-window when it
    // offers the manager (SerenityOS: Window::icon()). An invalid icon clears it.
    void set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const&);

    // Attach the client's shared bitmap (given as an fd) and commit. `size` is
    // the backing store size; `visible_size` is the part to present (they differ
    // during interactive resize, when LibGUI over-allocates by a margin).
    void attach_and_commit(i32 window_id, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha);

    // Reserve a strip at the top of the window (a menubar) drawn by `draw`,
    // with the client's content shifted down by `inset` pixels.
    void set_window_inset(i32 window_id, int inset, Function<void(Gfx::Bitmap&, Gfx::IntRect)> draw);

    // Record the regions an incoming paint will change, so the present can copy
    // and damage only those (empty = the whole window).
    void add_window_damage(i32 window_id, Vector<Gfx::IntRect> const& rects);

    // Popup surfaces for server-rendered menus. A submenu is parented to the
    // parent popup's surface and anchored at its item's right edge.
    void create_popup(i32 popup_id, i32 parent_window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu);
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
    void set_viewporter(wp_viewporter* viewporter) { m_viewporter = viewporter; }
    void set_layer_shell(zwlr_layer_shell_v1* shell) { m_layer_shell = shell; }
    void set_serenity_window_manager(serenity_window_manager* manager);
    // The window-server configuration the *desktop* owns: the workspace grid and
    // the input-setting bounds. The compositor sends it over serenity-window (see
    // the protocol and docs/24 in the serenity-desktop-environment repository), so
    // the shim answers an application's WindowServer config requests from this
    // rather than reading the desktop's config file.
    struct HostConfig {
        u32 workspace_rows { 1 };
        u32 workspace_columns { 1 };
        double accel_min { 0.5 };
        double accel_max { 3.5 };
        u32 scroll_step_min { 1 };
        int double_click_min { 100 };
        int double_click_max { 900 };
    };
    HostConfig const& host_config() const { return m_host_config; }
    // The desktop-owned settings, keyed semantically (see the protocol). The
    // compositor sends them; the shim answers an application's WindowServer config
    // requests from here and forwards changes back, so it never touches the
    // desktop's configuration files.
    ByteString host_setting(StringView key, StringView fallback = {}) const;
    void set_host_setting(StringView key, StringView value);
    // The desktop's wallpaper, sent as raw pixels over serenity-window.
    Gfx::Bitmap* host_wallpaper() { return m_host_wallpaper.ptr(); }
    void set_host_wallpaper(Gfx::Bitmap* bitmap);
    // The compositor asks the client to show/hide a window's menu bar.
    void set_menubar_visibility_callback(Function<void(i32 window_id, bool visible)> callback) { m_menubar_visibility_changed = move(callback); }
    // The compositor asks the client to close its open menus (see the protocol).
    void set_dismiss_menus_callback(Function<void()> callback) { m_dismiss_menus = move(callback); }
    // The compositor tells the client where it placed a window.
    void set_toplevel_position_callback(Function<void(i32 window_id, Gfx::IntPoint position)> callback) { m_toplevel_position_changed = move(callback); }
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
    void on_pointer_axis_delta(double x, double y);
    void on_keyboard_keymap(int fd, u32 size);
    void on_keyboard_modifiers(u32 depressed, u32 latched, u32 locked, u32 group, u32 serial);
    void on_keyboard_key(u32 key, bool pressed);
    void on_keyboard_focus(wl_surface*, bool entered);
    void on_toplevel_configure(xdg_toplevel*, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized, bool resizing);
    void on_toplevel_close(xdg_toplevel*);
    void on_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_layer_closed(zwlr_layer_surface_v1*);
    void on_menu_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_menu_layer_closed(zwlr_layer_surface_v1*);
    void on_menubar_visibility(serenity_toplevel*, bool visible);
    void on_toplevel_position(serenity_toplevel*, int32_t x, int32_t y);
    void on_dismiss_menus();
    void on_host_config(u32 workspace_rows, u32 workspace_columns, double accel_min, double accel_max, u32 scroll_step_min, int double_click_min, int double_click_max);
    void on_host_settings(StringView blob);
    void on_host_wallpaper(i32 width, i32 height, i32 pitch, u32 format, i32 fd);

    void on_data_offer(wl_data_offer*);
    void on_data_offer_mime(wl_data_offer*, char const* mime_type);
    void on_selection(wl_data_offer*);
    void on_source_send(wl_data_source*, char const* mime_type, int fd);
    void on_source_cancelled(wl_data_source*);
    // Explicit drag-and-drop (data-device DnD). The source starts a drag from the
    // focused window; the target receives enter/motion/drop with the mime data.
    bool start_drag(ByteString const& text, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& bitmap);
    void on_drag_enter(u32 serial, wl_data_offer*, wl_surface*, Gfx::IntPoint position);
    void on_drag_motion(Gfx::IntPoint position);
    void on_drag_drop();
    void on_drag_leave();
    void on_drag_source_cancelled(wl_data_source*);
    void on_drag_source_drop_performed();
    void on_drag_source_finished();
    void on_drag_source_action(u32 action) { m_drag_action = action; }
    void set_accepts_drag(bool accepts);
    void on_popup_done(xdg_popup*);
    void on_surface_configured(xdg_surface*);
    void on_frame_done(wl_callback*);
    void note_input_serial(u32 serial) { m_last_input_serial = serial; }

private:
    WaylandClient() = default;

    struct WindowSurface {
        i32 window_id { -1 };
        wl_surface* surface { nullptr };
        xdg_surface* xdg_surface_object { nullptr };
        xdg_toplevel* toplevel { nullptr };
        zxdg_toplevel_decoration_v1* decoration { nullptr };
        serenity_toplevel* chrome { nullptr };
        // Set for layer-surface windows (the Taskbar, the Desktop, applets):
        // these have no xdg_toplevel/xdg_surface, so their lifecycle differs.
        zwlr_layer_surface_v1* layer_surface { nullptr };
        bool is_layer { false };
        // A panel stretches along its anchored edges and reserves space.
        bool layer_panel { false };
        bool layer_configured { false };
        // The position the client asked for (unreliable for layer surfaces) and
        // the position the compositor actually gives the layer surface. Menus
        // are placed in output coordinates, so both are needed to translate the
        // client's anchor.
        Gfx::IntPoint requested_position;
        Gfx::IntPoint layer_output_position;
        // For ordinary xdg_toplevels (no layer_output_position): the compositor's
        // placement, from the serenity_toplevel.position event. Screen-relative
        // pointer coordinates are built from this.
        Gfx::IntPoint global_position;
        // An xdg_toplevel must not attach a buffer before its first configure.
        bool toplevel_configured { false };
        // Whether the compositor currently wants this window's menu bar shown.
        bool menubar_visible { true };
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
        // The composed buffer is reused across presents; it is only recreated
        // when the size/scale changes. `inset_dirty` forces the menubar strip to
        // be redrawn (and damaged) when it changes.
        bool composed_valid { false };
        bool inset_dirty { true };
        // Regions the next present must copy and damage (logical, visible
        // coordinates). Empty means "the whole window".
        Vector<Gfx::IntRect> damage;
        bool damage_full { false };
        // Compositor frame-callback pacing: while a frame is in flight we keep
        // only the newest client content and attach it when the frame lands.
        wl_callback* frame_callback { nullptr };
        bool frame_in_flight { false };
        int pending_fd { -1 };
        Gfx::IntSize pending_size;
        Gfx::IntSize pending_visible_size;
        i32 pending_pitch { 0 };
        bool pending_has_alpha { false };
        bool has_pending { false };
        // Interactive-resize stretch: while the compositor reports
        // XDG_TOPLEVEL_STATE_RESIZING we keep the last frame and let the
        // compositor scale it via a wp_viewport, avoiding a client re-render
        // per configure.
        wp_viewport* viewport { nullptr };
        bool resizing { false };
        bool stretch_active { false };
    };

    struct Popup {
        i32 id { -1 };
        wl_surface* surface { nullptr };
        xdg_surface* xdg_surface_object { nullptr };
        xdg_popup* popup { nullptr };
        // Menus shown from a layer-surface window (the Desktop, Taskbar, applets)
        // have no xdg parent, so they become overlay layer surfaces instead.
        zwlr_layer_surface_v1* layer_surface { nullptr };
        bool is_layer { false };
        Gfx::IntPoint layer_position;
        bool configured { false };
        Core::AnonymousBuffer buffer;
        RefPtr<Gfx::Bitmap> bitmap;
        RefPtr<Gfx::Bitmap> pending;
        Vector<NonnullOwnPtr<BufferRecord>> buffers;
    };

    WindowSurface* find(i32 window_id);
    void commit_window_content(WindowSurface&, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha);
    Popup* find_popup(i32 popup_id);
    i32 popup_id_for_surface(wl_surface*) const;
    void purge_released_buffers(WindowSurface&);
    void purge_released_buffers(Popup&);
    void bind_bitmap(wl_surface*, Core::AnonymousBuffer const&, Gfx::Bitmap const&, Vector<NonnullOwnPtr<BufferRecord>>&, Vector<Gfx::IntRect> const* damage = nullptr);
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
    wp_viewporter* m_viewporter { nullptr };
    zwlr_layer_shell_v1* m_layer_shell { nullptr };
    serenity_window_manager* m_serenity_window_manager { nullptr };
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
    // Drag-and-drop state (see on_drag_*).
    HashMap<String, ByteBuffer> m_drag_payload;
    u32 m_drag_action { 0 };
    wl_data_source* m_drag_source { nullptr };
    wl_surface* m_drag_icon { nullptr };
    wl_buffer* m_drag_icon_buffer { nullptr };
    Optional<Core::AnonymousBuffer> m_drag_icon_anon;
    wl_data_offer* m_drag_offer { nullptr };
    i32 m_drag_window { -1 };
    Gfx::IntPoint m_drag_position;
    u32 m_drag_serial { 0 };
    bool m_accepts_drag { false };
    bool m_drag_dropped { false };
    ByteString m_drag_mime;
    HashMap<String, ByteBuffer> m_drag_received;
    ByteBuffer receive_offer_data(wl_data_offer*, ByteString const& mime);
    Gfx::IntPoint adjusted_drag_position() const;
    u32 m_last_input_serial { 0 };
    u32 m_modifiers { 0 };
    Function<void(ByteString const&)> m_clipboard_changed;
    Function<void(i32 window_id, bool visible)> m_menubar_visibility_changed;
    Function<void(i32 window_id, Gfx::IntPoint)> m_toplevel_position_changed;
    Function<void()> m_dismiss_menus;
    HostConfig m_host_config;
    HashMap<ByteString, ByteString> m_host_settings;
    RefPtr<Gfx::Bitmap> m_host_wallpaper;

    InputCallbacks m_input;
    i32 m_pointer_window { -1 };
    i32 m_focused_window { -1 };
    Gfx::IntPoint m_pointer_position;
    u32 m_pointer_buttons { 0 };
    // Wayland axis deltas are often fractional (trackpads, high-resolution
    // wheels); accumulate them so whole scroll steps reach the GUI.
    double m_axis_accum_x { 0 };
    double m_axis_accum_y { 0 };

    Gfx::IntSize m_physical_size;
    Gfx::IntSize m_screen_size;
    int m_scale { 1 };
    int m_client_scale { 1 };
    bool m_have_logical_size { false };

    HashMap<i32, NonnullOwnPtr<WindowSurface>> m_windows;
    HashMap<i32, NonnullOwnPtr<Popup>> m_popups;
    i32 m_pointer_popup { -1 };
};

}
