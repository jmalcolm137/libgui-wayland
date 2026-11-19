/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Socket.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/Connection.h>
#include <LibIPC/File.h>
#include <WindowServer/ScreenLayout.h>
#include <WindowServer/WindowClientEndpoint.h>
#include <WindowServer/WindowServerEndpoint.h>

#include "MainThreadInvoker.h"
#include "MenuController.h"
#include "SerenityShell.h"
#include "WaylandClient.h"
#include "WindowServerDefaultStub.h"

namespace LibWM {

// The server end of the SerenityOS WindowServer protocol, backed by LibWM.
//
// One instance lives per application process (the in-process model). It decodes
// the client's commands via the generated WindowServer stub and drives the
// generated WindowClient proxy to deliver events back. Rendering/presentation
// is delegated to a presenter (currently a PNG dump; Wayland next).
class WindowServerConnection final
    : public IPC::Connection<WindowServerEndpoint, WindowClientEndpoint>
    , public WindowServerDefaultStub
    , public WindowClientEndpoint::Proxy<WindowServerEndpoint> {
    // Wayland events are dispatched on the main thread, but this connection, its
    // windows and its menus belong to the LibWM server thread, and LibIPC has no
    // send lock. Every callback handed to WaylandClient goes through here, so IPC
    // sends and state changes only ever happen on the server thread. Arguments
    // are moved into the posted task; they must not share refcounted data with
    // the caller (see copy_for_other_thread in WaylandClient.h).
    template<typename Callback>
    auto on_server_thread(Callback callback)
    {
        return [this, callback = move(callback)](auto... args) {
            if (MainThreadInvoker::on_server_thread()) {
                callback(move(args)...);
                return;
            }
            // Do not add a reference here. RefCountedBase is not atomic, and a ref
            // taken on this thread races the deref of the task's own capture on the
            // server thread: the lost update eventually drops the count to 0 and
            // deletes the connection under a live callback (VERIFICATION FAILED:
            // m_ref_count > 0 in on_toplevel_position). The connection is pinned for
            // the process lifetime by LibWM's leaked window_server_slot(), and the
            // server loop it posts to lives just as long, so the raw pointer is safe.
            MainThreadInvoker::post_to_server([this, callback, ... args = move(args)]() mutable {
                callback(move(args)...);
            });
        };
    }
    static void verify_on_server_thread() { VERIFY(MainThreadInvoker::on_server_thread()); }

public:
    static NonnullRefPtr<WindowServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new WindowServerConnection(move(socket)));
    }

    void send_fast_greet();
    void set_screen_size(Gfx::IntSize size) { m_screen_size = size; }
    Gfx::IntSize screen_size() const { return m_screen_size; }

private:
    explicit WindowServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<WindowServerEndpoint, WindowClientEndpoint>(*this, move(socket))
        , WindowClientEndpoint::Proxy<WindowServerEndpoint>(*this, {})
    {
        // This end lives on the LibWM server thread and writes input events and
        // replies back to the GUI thread. A blocking write here would stop it
        // reading the client's requests, so the two threads would deadlock.
        (void)this->socket().set_blocking(false);
        set_send_timeout(1000);

        // The current theme may be knowable only from the host settings -- there is no
        // live broker file unless a theme was changed this session -- and the settings
        // are delivered after this constructor runs. Recompute when they arrive.
        //
        // The *first* connection owns the hook: it is the app-facing one
        // (Application::create() creates it before LibWM's server thread exists). The
        // server-side connection is created later, on that thread, and used to take
        // the hook over -- which both aimed the recompute at an object belonging to
        // another thread and could leave this capture dangling once that object died.
        WaylandClient::the().serenity_shell().set_host_settings_changed_callback(this, [this] {
            MainThreadInvoker::post_to_server([this] {
                apply_theme_from_broker();
            });
        });
        watch_theme();
        watch_fonts();

        // Display links. Owned like the host-settings hook: it captures
        // `this`, so the first connection owns it and the destructor clears it.
        WaylandClient::the().set_display_link_callback(this, on_server_thread([this] { async_display_link_notification(); }));
    }

    ~WindowServerConnection()
    {
        // Drop the hooks if they are ours: they capture `this`, and settings and
        // frames keep arriving after a connection is torn down.
        WaylandClient::the().serenity_shell().clear_host_settings_changed_callback(this);
        WaylandClient::the().clear_display_link_callback(this);
    }

    // The WindowServer-protocol view of a window: what the client has been told
    // (its rect), the last backing store, and bookkeeping that has no Wayland
    // counterpart. State that is presented on Wayland (title, minimum size,
    // alpha, maximized) is owned by WaylandClient and queried from there.
    struct Window {
        i32 id { 0 };
        Gfx::IntRect rect;
        bool modified { false };
        // SerenityOS WindowMode (0 modeless .. 3 blocking) and the parent window
        // id (for a transient/modal dialog), from create_window.
        i32 window_mode { 0 };
        i32 parent_window_id { -1 };
        // True for popup-style windows (ComboBox lists, menus, tooltips), whose
        // position is chosen by the client. Normal toplevels and layer surfaces
        // are placed by the compositor, so a later client set_rect() must not
        // overwrite their real origin.
        bool client_positioned { false };
        RefPtr<Gfx::Bitmap> bitmap;
        i32 last_serial { -1 };
        // Coalesce configure-driven repaints: at most one resize+paint is in
        // flight until the client sends the next backing store.
        bool resize_in_flight { false };
        bool has_pending_resize { false };
        Gfx::IntSize pending_resize;
    };

    Window* window(i32 id);
    void remove_window(i32 id);
    void send_paint(Window&, Vector<Gfx::IntRect> rects);
    void request_window_resize(i32 window_id, Gfx::IntSize size);
    void flush_pending_resize(Window&);
    void present(Window&);
    void install_input_callbacks();
    void install_menu_callbacks();
    void update_window_menubar(i32 window_id);
    void present_menu_popup(i32 menu_id);

    // WindowServerEndpoint overrides (the commands LibGUI actually issues).
    void create_window(i32 window_id, i32 process_id, Gfx::IntRect const& rect, bool auto_position, bool has_alpha_channel, bool minimizable, bool closeable, bool resizable, bool fullscreen, bool frameless, bool forced_shadow, float alpha_hit_threshold, Gfx::IntSize base_size, Gfx::IntSize size_increment, Gfx::IntSize minimum_size, Optional<Gfx::IntSize> const& resize_aspect_ratio, i32 type, i32 mode, ByteString const& title, i32 parent_window_id, Gfx::IntRect const& launch_origin_rect) override;
    void set_window_title(i32 window_id, ByteString const& title) override;
    Messages::WindowServer::DestroyWindowResponse destroy_window(i32 window_id) override;
    Messages::WindowServer::SetWindowRectResponse set_window_rect(i32 window_id, Gfx::IntRect const& rect) override;
    Messages::WindowServer::GetWindowRectResponse get_window_rect(i32 window_id) override;
    Messages::WindowServer::GetWindowFloatingRectResponse get_window_floating_rect(i32 window_id) override;
    void set_window_minimum_size(i32 window_id, Gfx::IntSize size) override;
    Messages::WindowServer::GetWindowMinimumSizeResponse get_window_minimum_size(i32 window_id) override;
    Messages::WindowServer::GetWindowTitleResponse get_window_title(i32 window_id) override;
    void set_window_modified(i32 window_id, bool modified) override;
    Messages::WindowServer::IsWindowModifiedResponse is_window_modified(i32 window_id) override;
    Messages::WindowServer::GetAppletRectOnScreenResponse get_applet_rect_on_screen(i32 window_id) override;
    Messages::WindowServer::GetWindowRectFromClientResponse get_window_rect_from_client(i32 client_id, i32 window_id) override;
    void set_window_backing_store(i32 window_id, i32 bpp, i32 pitch, IPC::File const& anon_file, i32 serial, bool has_alpha_channel, Gfx::IntSize size, Gfx::IntSize visible_size, bool flush_immediately) override;
    void invalidate_rect(i32 window_id, Vector<Gfx::IntRect> const& rects, bool ignore_occlusion) override;
    void did_finish_painting(i32 window_id, Vector<Gfx::IntRect> const& rects) override;
    void set_window_has_alpha_channel(i32 window_id, bool has_alpha_channel) override;
    void set_window_alpha_hit_threshold(i32 window_id, float threshold) override;
    void set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const& icon) override;
    void set_window_cursor(i32 window_id, i32 cursor_type) override;
    void set_window_custom_cursor(i32 window_id, Gfx::ShareableBitmap const& cursor) override;
    void set_window_progress(i32 window_id, Optional<i32> const& progress) override;
    void set_fullscreen(i32 window_id, bool fullscreen) override;
    void set_frameless(i32 window_id, bool frameless) override;
    void set_maximized(i32 window_id, bool maximized) override;
    Messages::WindowServer::IsMaximizedResponse is_maximized(i32 window_id) override;
    void set_minimized(i32 window_id, bool minimized) override;
    void set_always_on_top(i32 window_id, bool always_on_top) override;
    void move_window_to_front(i32 window_id) override;
    Messages::WindowServer::GetGlobalCursorPositionResponse get_global_cursor_position() override;
    Messages::WindowServer::GetColorUnderCursorResponse get_color_under_cursor() override;
    Messages::WindowServer::GetWallpaperResponse get_wallpaper() override;
    void enable_display_link() override { WaylandClient::the().enable_display_link(); }
    void disable_display_link() override { WaylandClient::the().disable_display_link(); }
    Messages::WindowServer::GetScreenBitmapAroundCursorResponse get_screen_bitmap_around_cursor(Gfx::IntSize) override;
    Messages::WindowServer::GetScreenBitmapAroundLocationResponse get_screen_bitmap_around_location(Gfx::IntSize, Gfx::IntPoint) override;
    Messages::WindowServer::SetWallpaperResponse set_wallpaper(Gfx::ShareableBitmap const& wallpaper_bitmap) override;
    void set_wallpaper_mode(ByteString const& mode) override;
    void set_background_color(ByteString const& background_color) override;
    Messages::WindowServer::StartDragResponse start_drag(ByteString const& text, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& drag_bitmap) override;
    void set_accepts_drag(bool accepts) override;

    // Menus (server-rendered).
    void create_menu(i32 menu_id, String const& name, i32 minimum_width) override;
    void set_menu_name(i32 menu_id, String const& name) override;
    void set_menu_minimum_width(i32 menu_id, i32 minimum_width) override;
    void destroy_menu(i32 menu_id) override;
    void add_menu(i32 window_id, i32 menu_id) override;
    void add_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, Gfx::ShareableBitmap const& icon, bool exclusive) override;
    void add_menu_separator(i32 menu_id) override;
    void update_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, Gfx::ShareableBitmap const& icon) override;
    void remove_menu_item(i32 menu_id, i32 identifier) override;
    void flash_menubar_menu(i32 window_id, i32 menu_id) override;
    void popup_menu(i32 menu_id, Gfx::IntPoint screen_position, Gfx::IntRect const& button_rect) override;
    void dismiss_menu(i32 menu_id) override;

    // Theme (queried by the Taskbar's system menu).
    Messages::WindowServer::GetSystemThemeResponse get_system_theme() override;
    Messages::WindowServer::SetSystemThemeResponse set_system_theme(ByteString const& theme_path, ByteString const& theme_name, bool keep_desktop_background, Optional<ByteString> const& color_scheme_path) override;
    Messages::WindowServer::IsSystemThemeOverriddenResponse is_system_theme_overridden() override;
    Messages::WindowServer::GetPreferredColorSchemeResponse get_preferred_color_scheme() override;

    // Screen layout (queried by Display Settings). The host compositor presents
    // one output; configuration is accepted but not yet honoured.
    Messages::WindowServer::GetScreenLayoutResponse get_screen_layout() override;
    Messages::WindowServer::SetScreenLayoutResponse set_screen_layout(WindowServer::ScreenLayout const& screen_layout, bool save) override;
    Messages::WindowServer::SaveScreenLayoutResponse save_screen_layout() override;
    Messages::WindowServer::GetDesktopDisplayScaleResponse get_desktop_display_scale(u32 screen_index) override;
    void set_output_scale(i32 scale_v120) override;

    // Settings the host compositor accepts but does not act on (fonts are fixed to
    // the bundled bitmap family; workspaces are not implemented). Returning a valid
    // response matters: Serenity's Display Settings Apply makes these calls
    // synchronously, and an unhandled response blocks the application forever.
    Messages::WindowServer::SetSystemFontsResponse set_system_fonts(ByteString const& default_font_query, ByteString const& fixed_width_font_query, ByteString const& window_title_font_query) override;
    Messages::WindowServer::ApplyWorkspaceSettingsResponse apply_workspace_settings(u32 rows, u32 columns, bool save) override;
    Messages::WindowServer::GetWorkspaceSettingsResponse get_workspace_settings() override;
    Messages::WindowServer::GetSystemThemeOverrideResponse get_system_theme_override() override;
    Messages::WindowServer::SetSystemThemeOverrideResponse set_system_theme_override(Core::AnonymousBuffer const& buffer) override;
    void clear_system_theme_override() override;
    Messages::WindowServer::IsMinimizedResponse is_minimized(i32 window_id) override;
    Messages::WindowServer::GetScreenBitmapResponse get_screen_bitmap(Optional<Gfx::IntRect> const& rect, Optional<u32> const& screen_index) override;
    void set_window_parent_from_client(i32 client_id, i32 parent_id, i32 child_id) override;

    // Mouse/pointer settings (Mouse Settings). Backed by input.ini in the
    // shared state directory, which the compositor watches and applies to the
    // (libinput) devices.
    void set_mouse_acceleration(float factor) override;
    Messages::WindowServer::GetMouseAccelerationResponse get_mouse_acceleration() override;
    void set_scroll_step_size(u32 step_size) override;
    Messages::WindowServer::GetScrollStepSizeResponse get_scroll_step_size() override;
    void set_double_click_speed(int speed) override;
    Messages::WindowServer::GetDoubleClickSpeedResponse get_double_click_speed() override;
    void set_mouse_buttons_switched(bool switched) override;
    Messages::WindowServer::AreMouseButtonsSwitchedResponse are_mouse_buttons_switched() override;
    void set_natural_scroll(bool inverted) override;
    Messages::WindowServer::IsNaturalScrollResponse is_natural_scroll() override;
    Messages::WindowServer::GetMouseAccelerationMinResponse get_mouse_acceleration_min() override;
    Messages::WindowServer::GetMouseAccelerationMaxResponse get_mouse_acceleration_max() override;
    Messages::WindowServer::GetScrollStepSizeMinResponse get_scroll_step_size_min() override;
    Messages::WindowServer::GetDoubleClickSpeedMinResponse get_double_click_speed_min() override;
    Messages::WindowServer::GetDoubleClickSpeedMaxResponse get_double_click_speed_max() override;

    // Cursor theme/highlight (Mouse Settings; the SerenityDE compositor applies
    // them). Backed by cursor.ini in the state directory.
    Messages::WindowServer::GetCursorThemeResponse get_cursor_theme() override;
    void apply_cursor_theme(ByteString const& name) override;
    Messages::WindowServer::GetCursorHighlightRadiusResponse get_cursor_highlight_radius() override;
    void set_cursor_highlight_radius(int radius) override;
    Messages::WindowServer::GetCursorHighlightColorResponse get_cursor_highlight_color() override;
    void set_cursor_highlight_color(Gfx::Color color) override;

    // Keyboard layouts (Keyboard Settings). Backed by input.ini [Keyboard].
    Messages::WindowServer::GetKeyboardLayoutResponse get_keyboard_layout() override;
    void set_keyboard_layout(ByteString const& layouts, ByteString const& variant, ByteString const& model, ByteString const& options) override;
    Messages::WindowServer::GetKeyboardLayoutNamesResponse get_keyboard_layout_names() override;

    // Effects (Display Settings ▸ Effects): apply to this client and record
    // the choices in WindowServer.ini [Effects] for the compositor and others.
    void set_system_effects(Vector<bool> const& effects, u8 geometry, u8 tile_window) override;

    // Broadcasts theme changes between processes (each app has its own
    // WindowServer shim) via a shared file the compositor and clients watch.
    void watch_theme();
    void apply_theme_from_broker();
    ByteString m_theme_name { "Default" };
    // ThemeEditor's live preview. It restyles this application only: other
    // applications are separate processes with their own LibWM.
    Optional<Core::AnonymousBuffer> m_theme_override;
    Optional<Core::AnonymousBuffer> m_theme_before_override;
    RefPtr<Core::FileWatcher> m_theme_watcher;

    // System fonts: a session broker ($XDG_RUNTIME_DIR/sde-fonts), like the
    // theme broker, so a font change applies to every client and the compositor
    // Also what makes size-based zoom work with a scalable family.
    struct FontQueries {
        ByteString default_query;
        ByteString fixed_width_query;
        ByteString window_title_query;
    };
    static FontQueries read_fonts_broker();
    void watch_fonts();
    void apply_fonts_from_broker();
    RefPtr<Core::FileWatcher> m_fonts_watcher;

    // Wallpaper/background: the shim persists the chosen image, mode and colour
    // to a shared state directory the compositor watches (no cross-process bus).
    // The bitmap is loaded on demand, not pushed to every client.
    RefPtr<Gfx::Bitmap> m_wallpaper;
    ByteString m_wallpaper_mode;

    Vector<NonnullOwnPtr<Window>> m_windows;
    Gfx::IntSize m_screen_size { 1280, 800 };
    i32 m_client_id { 1 };
    i32 m_active_window_id { -1 };
    // The window that last received a button press; used as the parent for a
    // menu shown from a button or context menu when the menu has no owner window.
    i32 m_last_mouse_down_window_id { -1 };
    MenuController m_menu;

    // Double-click detection, mirroring WindowServer::WindowManager: a second
    // press/release within the interval and distance is delivered to the client
    // as a MouseDoubleClick (after the MouseUp).
    struct DoubleClickMetadata {
        Core::ElapsedTimer clock;
        Gfx::IntPoint last_position;
    };
    i32 m_double_click_window { -1 };
    HashMap<u32, DoubleClickMetadata> m_double_click_metadata;
    int m_double_click_speed { 250 };
    int m_max_distance_for_double_click { 4 };
};

}
