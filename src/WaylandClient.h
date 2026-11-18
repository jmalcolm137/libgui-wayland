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
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>

#include <pthread.h>

struct wl_buffer;
struct wl_callback;
struct wl_compositor;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
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
struct serenity_toplevel;

namespace Core {
class Notifier;
}

namespace LibWM {

class SerenityShell;
class WaylandDataDevice;
class WaylandOutputs;
class WaylandSeat;

// The connection to the real Wayland compositor.
//
// This is the source of truth for environmental state (principle P1). It owns
// the wl_display, the registry and core globals, the windows and menu popups
// (WaylandSurface.cpp, WaylandPopup.cpp), and one object per protocol area:
//
//   outputs()        WaylandOutputs     screen size, output and client scales
//   seat()           WaylandSeat        pointer, keyboard, text input, cursor
//   data_device()    WaylandDataDevice  clipboard and drag-and-drop
//   serenity_shell() SerenityShell      the optional SerenityDE integration
//
// Those are friends: they share this object's mutex, globals and window table.
//
// Threading: Wayland events are dispatched on the main thread, while the
// in-process portal servers (WindowServerConnection, ClipboardServerConnection)
// invoke methods from the LibWM server thread. Access to shared state is
// serialized by the recursive mutex `m_mutex` (and per-surface
// `WindowSurface::mutex`, always taken after `m_mutex`).
class WaylandClient {
public:
    struct MutexLocker {
        explicit MutexLocker(pthread_mutex_t& mutex)
            : m_mutex(mutex)
        {
            pthread_mutex_lock(&m_mutex);
        }
        ~MutexLocker() { pthread_mutex_unlock(&m_mutex); }

    private:
        pthread_mutex_t& m_mutex;
    };

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
        Function<void(i32 popup_id, i32 wheel_delta_y)> popup_wheel;
        Function<void(i32 popup_id)> popup_closed;
    };

    static WaylandClient& the();

    ErrorOr<void> ensure_connected();
    bool is_connected() const
    {
        MutexLocker locker(m_mutex);
        return m_display != nullptr;
    }

    // Screen size and output/client scales (wl_output, zxdg_output_v1).
    WaylandOutputs& outputs() { return *m_outputs; }
    WaylandOutputs const& outputs() const { return *m_outputs; }

    // Pointer, keyboard, text input and the cursor (wl_seat).
    WaylandSeat& seat() { return *m_seat; }

    void set_input_callbacks(InputCallbacks callbacks)
    {
        MutexLocker locker(m_mutex);
        m_input = move(callbacks);
    }

    // The compositor connection failed (closed socket, protocol error, or a
    // vanished compositor). Without a handler the current event loop is asked to
    // exit, which is right for a single-window applet. An application that owns
    // durable state (the browser session) installs a handler to record why it
    // ended and save before quitting.
    void set_connection_lost_callback(Function<void(ByteString const& reason)> callback)
    {
        MutexLocker locker(m_mutex);
        m_connection_lost = move(callback);
    }

    // Clipboard and drag-and-drop (wl_data_device).
    WaylandDataDevice& data_device() { return *m_data_device; }

    // Window <-> xdg_toplevel lifecycle.
    void create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize, ByteString const& title, bool has_alpha, bool resizable, bool frameless, i32 window_type, i32 window_mode, i32 parent_window_id, Gfx::IntRect const& launch_origin = {});
    void destroy_window(i32 window_id);
    void set_title(i32 window_id, ByteString const& title);

    // The per-window state presented on Wayland is owned here; the WindowServer
    // connection answers the client's queries from these rather than keeping
    // copies. Unknown windows yield empty values.
    ByteString window_title(i32 window_id) const;
    Gfx::IntSize window_minimum_size(i32 window_id) const;
    bool window_has_alpha(i32 window_id) const;

    // Whether the compositor currently wants the window's menu bar shown.
    bool window_menubar_visible(i32 window_id) const;

    // Update the size of a layer-surface window (the panel height, say). The
    // compositor controls the position of layer surfaces; for ordinary xdg
    // toplevels this is a no-op (their size is driven by a configure).
    void set_window_rect(i32 window_id, Gfx::IntSize size);

    // Window state (maps to xdg_toplevel).
    void set_fullscreen(i32 window_id, bool fullscreen);
    void set_maximized(i32 window_id, bool maximized);
    bool is_window_maximized(i32 window_id) const;
    void set_minimized(i32 window_id);
    void set_always_on_top(i32 window_id, bool always_on_top);
    void set_minimum_size(i32 window_id, Gfx::IntSize size);
    void set_frameless(i32 window_id, bool frameless);
    void set_window_has_alpha_channel(i32 window_id, bool has_alpha);
    void set_window_alpha_hit_threshold(i32 window_id, float threshold);
    void set_window_cursor(i32 window_id, i32 cursor_type);
    void set_window_custom_cursor(i32 window_id, Gfx::ShareableBitmap const& cursor);

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
    bool window_uses_menubar_subsurface(i32 window_id) const;

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

    // --- registry globals and protocol events (called by the C listeners) ---
    struct BufferRecord {
        wl_buffer* buffer { nullptr };
        bool released { false };
    };
    void set_compositor(wl_compositor* compositor)
    {
        MutexLocker locker(m_mutex);
        m_compositor = compositor;
    }
    void set_subcompositor(wl_subcompositor* subcompositor)
    {
        MutexLocker locker(m_mutex);
        m_subcompositor = subcompositor;
    }
    void set_shm(wl_shm* shm)
    {
        MutexLocker locker(m_mutex);
        m_shm = shm;
    }
    void add_wm_base(xdg_wm_base* wm_base);
    void set_decoration_manager(zxdg_decoration_manager_v1* manager)
    {
        MutexLocker locker(m_mutex);
        m_decoration_manager = manager;
    }
    void set_viewporter(wp_viewporter* viewporter)
    {
        MutexLocker locker(m_mutex);
        m_viewporter = viewporter;
    }
    void set_fractional_scale_manager(wp_fractional_scale_manager_v1* manager)
    {
        MutexLocker locker(m_mutex);
        m_fractional_scale_manager = manager;
    }
    void on_preferred_scale(wp_fractional_scale_v1*, u32 scale_v120);
    void set_layer_shell(zwlr_layer_shell_v1* shell)
    {
        MutexLocker locker(m_mutex);
        m_layer_shell = shell;
    }
    // The SerenityDE integration (serenity-window protocol): desktop-owned
    // settings, wallpaper, capture, and compositor-driven window chrome. Inert on
    // other compositors.
    SerenityShell& serenity_shell() { return *m_serenity_shell; }

    // GUI::DisplayLink. SerenityOS drives it from the window server's frame loop;
    // here it is paced by a window surface's frame callbacks: the callback runs
    // once per compositor frame while enabled. The callback belongs to `owner`:
    // it is installed only if none is set, and only its owner can clear it.
    void set_display_link_callback(void* owner, Function<void()> callback);
    void clear_display_link_callback(void* owner);
    void enable_display_link();
    void disable_display_link();

    i32 window_id_for_surface(wl_surface*) const;
    void on_toplevel_configure(xdg_toplevel*, Gfx::IntSize size, bool activated, bool fullscreen, bool maximized, bool resizing);
    void on_decoration_configure(zxdg_toplevel_decoration_v1*, u32 mode);
    void on_toplevel_close(xdg_toplevel*);
    void on_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_layer_closed(zwlr_layer_surface_v1*);
    void on_menu_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_menu_layer_closed(zwlr_layer_surface_v1*);

    void on_popup_done(xdg_popup*);
    void on_surface_configured(xdg_surface*);
    void on_frame_done(wl_callback*);
    void on_display_link_frame_done(wl_callback*);

private:
    friend class SerenityShell;
    friend class WaylandDataDevice;
    friend class WaylandOutputs;
    friend class WaylandSeat;

    WaylandClient();
    ~WaylandClient();

    mutable pthread_mutex_t m_mutex {};

    struct WindowSurface {
        // The Wayland display is dispatched on the main thread, but the LibWM
        // server thread also calls into this surface (a client's paint request and
        // backing-store handover arrive over its WindowServer connection). Both
        // sides mutate the same damage/pending/composed state, so every entry
        // point that touches a surface holds this lock.
        pthread_mutex_t mutex {};
        WindowSurface()
        {
            pthread_mutexattr_t attributes;
            pthread_mutexattr_init(&attributes);
            pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&mutex, &attributes);
            pthread_mutexattr_destroy(&attributes);
        }
        ~WindowSurface() { pthread_mutex_destroy(&mutex); }
        static constexpr int csd_titlebar_pixels = 22;
        int csd_height() const
        {
            return (use_csd && !frameless && !fullscreen && !is_layer) ? csd_titlebar_pixels : 0;
        }
        int total_top_inset() const
        {
            return csd_height() + inset;
        }

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
        Gfx::IntSize minimum_size;
        bool has_alpha { false };
        float alpha_hit_threshold { 0.0f };
        bool input_region_active { false };
        // The opaque region last sent to the compositor (empty = none).
        Gfx::IntSize opaque_region_size;
        bool resizable { true };
        bool frameless { false };
        bool fullscreen { false };
        bool maximized { false };
        bool activated { true };
        bool use_csd { false };
        bool csd_dirty { true };
        i32 cursor_type { 0 };
        RefPtr<Gfx::Bitmap> custom_cursor;
        RefPtr<Gfx::Bitmap> icon_bitmap;
        Gfx::IntSize fixed_size;
        Vector<NonnullOwnPtr<BufferRecord>> buffers;
        // Optional top strip (menubar). The direct path presents it from a small
        // subsurface placed at y = -inset; the compositor offsets the client
        // content down by the same amount. The composed_* fields are the
        // compatibility fallback for compositors without a subcompositor or a
        // version-2 serenity_toplevel.
        int inset { 0 };
        Function<void(Gfx::Bitmap&, Gfx::IntRect)> draw_inset;
        wl_surface* menubar_surface { nullptr };
        wl_subsurface* menubar_subsurface { nullptr };
        Core::AnonymousBuffer menubar_buffer;
        RefPtr<Gfx::Bitmap> menubar_bitmap;
        Vector<NonnullOwnPtr<BufferRecord>> menubar_buffers;
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
        // Interactive-resize stretch and fractional-scale presentation via wp_viewport.
        wp_viewport* viewport { nullptr };
        wp_fractional_scale_v1* fractional_scale { nullptr };
        Core::AnonymousBuffer fractional_buffer;
        RefPtr<Gfx::Bitmap> fractional_bitmap;
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

    struct DisplayLinkState {
        bool enabled { false };
        Function<void()> callback_function;
        void* callback_owner { nullptr };
        wl_callback* callback { nullptr };
        wl_surface* surface { nullptr };
    };

    WindowSurface* find(i32 window_id);
    void commit_window_content(WindowSurface&, int client_fd, Gfx::IntSize size, Gfx::IntSize visible_size, i32 pitch, bool has_alpha);
    void update_opaque_region(WindowSurface&, Gfx::IntSize logical_size);
    static bool needs_input_region_pixels(WindowSurface const&);
    void update_input_region(WindowSurface&, u8 const* client_pixels, size_t pitch, Gfx::IntSize visible_size, int scale, int total_inset);
    void present_menubar(WindowSurface&, Gfx::IntSize visible_size);
    void render_csd_titlebar(WindowSurface const&, Gfx::Bitmap&, int width);
    Popup* find_popup(i32 popup_id);
    i32 popup_id_for_surface(wl_surface*) const;
    void purge_released_buffers(WindowSurface&);
    void purge_released_buffers(Popup&);
    static void purge_released_buffers(Vector<NonnullOwnPtr<BufferRecord>>&);
    void bind_bitmap(wl_surface*, Core::AnonymousBuffer const&, Gfx::Bitmap const&, Vector<NonnullOwnPtr<BufferRecord>>&, Vector<Gfx::IntRect> const* damage = nullptr);
    void request_display_link_frame();

    wl_display* m_display { nullptr };
    wl_registry* m_registry { nullptr };
    wl_compositor* m_compositor { nullptr };
    wl_subcompositor* m_subcompositor { nullptr };
    wl_shm* m_shm { nullptr };
    xdg_wm_base* m_wm_base { nullptr };
    zxdg_decoration_manager_v1* m_decoration_manager { nullptr };
    wp_viewporter* m_viewporter { nullptr };
    wp_fractional_scale_manager_v1* m_fractional_scale_manager { nullptr };
    zwlr_layer_shell_v1* m_layer_shell { nullptr };
    RefPtr<Core::Notifier> m_notifier;

    DisplayLinkState m_display_link;

    NonnullOwnPtr<SerenityShell> m_serenity_shell;
    NonnullOwnPtr<WaylandOutputs> m_outputs;
    NonnullOwnPtr<WaylandDataDevice> m_data_device;
    NonnullOwnPtr<WaylandSeat> m_seat;

    InputCallbacks m_input;
    Function<void(ByteString const&)> m_connection_lost;

    HashMap<i32, NonnullOwnPtr<WindowSurface>> m_windows;
    HashMap<i32, NonnullOwnPtr<Popup>> m_popups;
};

}
