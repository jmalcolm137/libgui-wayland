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
#include <LibCore/Socket.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/Connection.h>
#include <LibIPC/File.h>
#include <WindowServer/ScreenLayout.h>
#include <WindowServer/WindowClientEndpoint.h>
#include <WindowServer/WindowServerEndpoint.h>

#include "WindowServerDefaultStub.h"
#include "MenuController.h"

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
    }

    struct Window {
        i32 id { 0 };
        Gfx::IntRect rect;
        Gfx::IntSize minimum_size;
        bool has_alpha_channel { false };
        ByteString title;
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
    Messages::WindowServer::IsWindowModifiedResponse is_window_modified(i32 window_id) override;
    Messages::WindowServer::GetAppletRectOnScreenResponse get_applet_rect_on_screen(i32 window_id) override;
    Messages::WindowServer::GetWindowRectFromClientResponse get_window_rect_from_client(i32 client_id, i32 window_id) override;
    void set_window_backing_store(i32 window_id, i32 bpp, i32 pitch, IPC::File const& anon_file, i32 serial, bool has_alpha_channel, Gfx::IntSize size, Gfx::IntSize visible_size, bool flush_immediately) override;
    void invalidate_rect(i32 window_id, Vector<Gfx::IntRect> const& rects, bool ignore_occlusion) override;
    void did_finish_painting(i32 window_id, Vector<Gfx::IntRect> const& rects) override;
    void set_window_has_alpha_channel(i32 window_id, bool has_alpha_channel) override;
    void set_window_alpha_hit_threshold(i32 window_id, float threshold) override;
    void set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const& icon) override;
    void set_window_progress(i32 window_id, Optional<i32> const& progress) override;
    void set_fullscreen(i32 window_id, bool fullscreen) override;
    void set_maximized(i32 window_id, bool maximized) override;
    void set_minimized(i32 window_id, bool minimized) override;
    void move_window_to_front(i32 window_id) override;
    Messages::WindowServer::GetGlobalCursorPositionResponse get_global_cursor_position() override;
    Messages::WindowServer::GetColorUnderCursorResponse get_color_under_cursor() override;
    Messages::WindowServer::GetWallpaperResponse get_wallpaper() override;
    Messages::WindowServer::SetWallpaperResponse set_wallpaper(Gfx::ShareableBitmap const& wallpaper_bitmap) override;
    Messages::WindowServer::StartDragResponse start_drag(ByteString const& text, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& drag_bitmap) override;

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

    Vector<NonnullOwnPtr<Window>> m_windows;
    Gfx::IntSize m_screen_size { 1280, 800 };
    i32 m_client_id { 1 };
    i32 m_active_window_id { -1 };
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
