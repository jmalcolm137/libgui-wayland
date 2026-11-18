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
struct wl_keyboard;
struct wl_output;
struct wl_pointer;
struct wl_registry;
struct wl_seat;
struct zwp_text_input_manager_v3;
struct zwp_text_input_v3;
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

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace Core {
class Notifier;
}

namespace Gfx {
enum class StandardCursor;
}

namespace LibWM {

class SerenityShell;

// A thin wrapper over the real Wayland compositor.
//
// This is the source of truth for environmental state (principle P1): output
// logical size and scale, seat capabilities, input, shm, and the xdg-shell
// objects used to present LibGUI windows.
//
// Threading: Wayland events are dispatched on the main thread, while the
// in-process portal servers (WindowServerConnection, ClipboardServerConnection)
// invoke methods from the LibWM server thread. Access to shared state is
// serialized by the recursive mutex `m_mutex` (and per-surface `WindowSurface::mutex`).
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

    // Logical screen size and output scale as reported by the compositor.
    Gfx::IntSize screen_size() const
    {
        MutexLocker locker(m_mutex);
        return m_output_state.screen_size;
    }
    int output_scale() const
    {
        MutexLocker locker(m_mutex);
        return m_output_state.scale;
    }
    int output_scale_v120() const
    {
        MutexLocker locker(m_mutex);
        return m_output_state.scale_v120;
    }
    // The integer backing-store scale the shim told the client to render at
    // (ceil(client_scale_v120 / 120.0); 1 when client scaling is disabled).
    int client_scale() const
    {
        MutexLocker locker(m_mutex);
        return m_output_state.client_scale;
    }
    int client_scale_v120() const
    {
        MutexLocker locker(m_mutex);
        return m_output_state.client_scale_v120;
    }
    void set_client_scale(int scale)
    {
        MutexLocker locker(m_mutex);
        int clamped = scale < 1 ? 1 : scale;
        m_output_state.client_scale = clamped;
        m_output_state.client_scale_v120 = clamped * 120;
    }
    void set_client_scale_v120(int scale_v120)
    {
        MutexLocker locker(m_mutex);
        int clamped = scale_v120 < 60 ? 120 : scale_v120;
        m_output_state.client_scale_v120 = clamped;
        m_output_state.client_scale = max(1, (clamped + 119) / 120);
    }

    // The pointer's position in output coordinates, reconstructed from the
    // surface under the pointer and that surface's known origin. The compositor
    // owns window placement (and never reports the cursor directly), so this is
    // how the client places tooltips near the cursor.
    Gfx::IntPoint global_pointer_position();

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

    // --- Clipboard (native Wayland data device) ---
    // Called when the compositor's selection changes; the mime type is a
    // preferred Serenity-side type (e.g. "text/plain").
    void set_clipboard_changed_callback(Function<void(ByteString const&)> callback)
    {
        MutexLocker locker(m_mutex);
        m_data_device.on_clipboard_changed = move(callback);
    }
    // The MIME types the current selection offers, in the source's order.
    Vector<ByteString> clipboard_mime_types() const;
    // Fetch the current selection, preferring text then files/images. Blocks until the source closes the pipe.
    ErrorOr<ByteBuffer> read_clipboard(ByteString& out_mime_type);
    // Fetch one specific representation of the current selection. Blocks until the source closes the pipe.
    ErrorOr<ByteBuffer> read_clipboard_for_mime(ByteString const& mime_type, ByteString& out_mime_type);
    // Advertise a set of mime-type -> bytes representations as the selection.
    void write_clipboard(HashMap<ByteString, ByteBuffer> offers);
    // Whether this client currently owns the selection.
    bool owns_clipboard() const
    {
        MutexLocker locker(m_mutex);
        return m_data_device.selection_source && !m_data_device.clipboard_offers.is_empty();
    }

    // Window <-> xdg_toplevel lifecycle.
    void create_window(i32 window_id, Gfx::IntPoint position, Gfx::IntSize, ByteString const& title, bool has_alpha, bool resizable, bool frameless, i32 window_type, i32 window_mode, i32 parent_window_id, Gfx::IntRect const& launch_origin = {});
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

    // --- called by the C protocol listeners ---
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
    void add_seat(wl_seat* seat);
    void ensure_text_input();
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
    void set_data_device_manager(wl_data_device_manager* manager);

    // --- Text input / IME (zwp_text_input_v3) ---
    // The compositor relays preedit and committed text from the input method to the focused client. The app enables
    // text input only while a text field has focus, and supplies the caret rectangle so candidate windows can follow it.
    using TextInputCommit = Function<void(String const&)>;
    using TextInputPreedit = Function<void(String const&, i32 cursor_begin, i32 cursor_end)>;
    using TextInputDeleteSurrounding = Function<void(i32 before_length, i32 after_length)>;
    void set_text_input_callbacks(TextInputCommit, TextInputPreedit, TextInputDeleteSurrounding);
    void set_text_input_enabled(bool);
    bool text_input_enabled() const
    {
        MutexLocker locker(m_mutex);
        return m_text_input.enabled;
    }
    void set_text_input_cursor_rect(Gfx::IntRect const& rect);
    void set_text_input_manager(zwp_text_input_manager_v3*);
    void on_text_input_enter();
    void on_text_input_commit(String);
    void on_text_input_preedit(String, i32 cursor_begin, i32 cursor_end);
    void on_text_input_delete_surrounding(i32 before_length, i32 after_length);
    void add_output(wl_output* output);
    void set_xdg_output_manager(zxdg_output_manager_v1* manager);
    void on_output_logical_size(wl_output*, Gfx::IntSize logical_size);
    void on_output_mode(Gfx::IntSize physical_size)
    {
        MutexLocker locker(m_mutex);
        m_output_state.physical_size = physical_size;
    }
    void on_output_scale(int factor)
    {
        MutexLocker locker(m_mutex);
        m_output_state.scale = factor > 0 ? factor : 1;
        if (!m_output_state.have_logical_size)
            m_output_state.scale_v120 = m_output_state.scale * 120;
    }
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
    void on_decoration_configure(zxdg_toplevel_decoration_v1*, u32 mode);
    void on_toplevel_close(xdg_toplevel*);
    void on_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_layer_closed(zwlr_layer_surface_v1*);
    void on_menu_layer_configure(zwlr_layer_surface_v1*, Gfx::IntSize size);
    void on_menu_layer_closed(zwlr_layer_surface_v1*);

    void on_data_offer(wl_data_offer*);
    void on_data_offer_mime(wl_data_offer*, char const* mime_type);
    void on_selection(wl_data_offer*);
    // Drop an offer we are about to destroy from the selection state (the drag and
    // the selection share the pending-offer list).
    void forget_offer(wl_data_offer*);
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
    void on_drag_source_action(u32 action)
    {
        MutexLocker locker(m_mutex);
        m_data_device.drag_action = action;
    }
    void set_accepts_drag(bool accepts);
    void on_popup_done(xdg_popup*);
    void on_surface_configured(xdg_surface*);
    void on_frame_done(wl_callback*);
    void on_display_link_frame_done(wl_callback*);
    void note_input_serial(u32 serial)
    {
        MutexLocker locker(m_mutex);
        m_input_state.last_serial = serial;
    }
    void note_pointer_enter_serial(u32 serial)
    {
        MutexLocker locker(m_mutex);
        m_input_state.last_serial = serial;
        m_input_state.pointer_enter_serial = serial;
    }

private:
    friend class SerenityShell;

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

    struct OutputState {
        wl_output* primary { nullptr };
        Vector<wl_output*> outputs;
        zxdg_output_manager_v1* xdg_manager { nullptr };
        Gfx::IntSize physical_size;
        Gfx::IntSize screen_size;
        int scale { 1 };
        int scale_v120 { 120 };
        int client_scale { 1 };
        int client_scale_v120 { 120 };
        bool have_logical_size { false };
    };

    struct CachedCursor {
        RefPtr<Gfx::Bitmap> bitmap;
        Gfx::IntPoint hotspot;
    };

    struct InputState {
        wl_seat* seat { nullptr };
        wl_pointer* pointer { nullptr };
        wl_keyboard* keyboard { nullptr };
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
        Vector<NonnullOwnPtr<BufferRecord>> cursor_buffers;
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

    struct DataDeviceState {
        wl_data_device_manager* manager { nullptr };
        wl_data_device* device { nullptr };
        wl_data_offer* current_offer { nullptr };
        Vector<wl_data_offer*> pending_offers;
        HashMap<u64, Vector<ByteString>> offer_mime_types;
        wl_data_source* selection_source { nullptr };
        HashMap<ByteString, ByteBuffer> clipboard_offers;
        Function<void(ByteString const&)> on_clipboard_changed;
        // Drag-and-drop state (see on_drag_*).
        HashMap<String, ByteBuffer> drag_payload;
        u32 drag_action { 0 };
        wl_data_source* drag_source { nullptr };
        wl_surface* drag_icon { nullptr };
        wl_buffer* drag_icon_buffer { nullptr };
        Optional<Core::AnonymousBuffer> drag_icon_anon;
        wl_data_offer* drag_offer { nullptr };
        i32 drag_window { -1 };
        Gfx::IntPoint drag_position;
        u32 drag_serial { 0 };
        bool accepts_drag { false };
        bool drag_dropped { false };
        ByteString drag_mime;
        HashMap<String, ByteBuffer> drag_received;
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
    void apply_window_cursor(WindowSurface const&);
    Optional<CachedCursor> load_standard_cursor(Gfx::StandardCursor);
    Popup* find_popup(i32 popup_id);
    i32 popup_id_for_surface(wl_surface*) const;
    void purge_released_buffers(WindowSurface&);
    void purge_released_buffers(Popup&);
    static void purge_released_buffers(Vector<NonnullOwnPtr<BufferRecord>>&);
    void bind_bitmap(wl_surface*, Core::AnonymousBuffer const&, Gfx::Bitmap const&, Vector<NonnullOwnPtr<BufferRecord>>&, Vector<Gfx::IntRect> const* damage = nullptr);
    u32 current_modifiers() const;
    void maybe_create_data_device();
    ByteString preferred_mime_for(wl_data_offer*) const;
    ByteBuffer receive_offer_data(wl_data_offer*, ByteString const& mime);
    Gfx::IntPoint adjusted_drag_position() const;
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

    OutputState m_output_state;
    InputState m_input_state;
    TextInputState m_text_input;
    DataDeviceState m_data_device;
    DisplayLinkState m_display_link;

    NonnullOwnPtr<SerenityShell> m_serenity_shell;

    InputCallbacks m_input;
    Function<void(ByteString const&)> m_connection_lost;

    HashMap<i32, NonnullOwnPtr<WindowSurface>> m_windows;
    HashMap<i32, NonnullOwnPtr<Popup>> m_popups;
};

}
