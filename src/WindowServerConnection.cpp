/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowServerConnection.h"
#include "WaylandClient.h"
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGUI/Window.h>
#include <WindowServer/SystemEffects.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

// Wallpaper/background state: written by apps, read by the compositor. Kept in a
// persistent directory so a chosen wallpaper survives a session restart.




// Input settings (R4.6): a small input.ini the compositor watches and applies to
// the (libinput) devices. Mouse Settings reads and writes it through here.


// Cursor theme/highlight (cursor.ini): Mouse Settings' Highlight/Theme tabs. The
// compositor applies these to the real cursor/highlight in R4.7.


// The SDE WindowServer.ini (the compositor's config schema): effects, input
// ranges, workspace grid, ... The port reads it for values clients need and
// writes the [Effects] section back when the user changes effects.



// The wallpaper state is stored as raw pixels, not a PNG: encoding a full-screen
// image to PNG takes several hundred milliseconds and would stall the UI thread
// of whichever application applies it. Must match WallpaperHeader in the
// compositor (WindowServer/src/main.cpp).
struct WallpaperHeader {
    u32 magic;
    u32 format;
    u32 width;
    u32 height;
    u32 pitch;
};
constexpr u32 wallpaper_magic = 0x53445750; // "SDWP"



}

namespace LibWM {

WindowServerConnection::Window* WindowServerConnection::window(i32 id)
{
    for (auto& window : m_windows) {
        if (window->id == id)
            return window.ptr();
    }
    return nullptr;
}

void WindowServerConnection::remove_window(i32 id)
{
    m_windows.remove_first_matching([&](auto& window) { return window->id == id; });
}

// The workspace grid, read from the same config the compositor uses ([Workspaces]
// in WindowServer.ini: the SerenityOS default overlaid by the user's file in the
// session state directory). GUI::Desktop gets these with the screen rects, and the
// WorkspacePicker applet sizes its grid from them.


// R4.6: input-setting bounds from WindowServer.ini [Input] (the SDE config
// schema, the same file the compositor reads). Returns the raw entry so callers
// can parse the type they need.

namespace {
struct FontQueries {
    ByteString default_query;
    ByteString fixed_width_query;
    ByteString window_title_query;
};
FontQueries read_fonts_broker();
}

void WindowServerConnection::send_fast_greet()
{
    install_input_callbacks();
    install_menu_callbacks();

    auto& wayland = WaylandClient::the();
    if (auto result = wayland.ensure_connected(); result.is_error())
        dbgln("LibWM: running without a Wayland compositor: {}", result.error());

    auto screen = wayland.screen_size();
    if (screen.is_empty())
        screen = m_screen_size;

    Vector<Gfx::IntRect> screen_rects;
    screen_rects.append({ 0, 0, screen.width(), screen.height() });

    // The client indexes this vector by SystemEffects::Effects, so it must have
    // exactly __Count entries (an empty vector causes an out-of-bounds crash on
    // the first system_effects() access). The animated effects stay off; tooltips
    // are enabled (WS6) so Application::show_tooltip() creates a tooltip window.
    Vector<bool> effects;
    for (size_t i = 0; i < to_underlying(WindowServer::Effects::__Count); ++i)
        effects.append(false);
    effects[to_underlying(WindowServer::Effects::Tooltips)] = true;

    // The compositor owns the grid and sends it over serenity-window; the shim
    // does not read the desktop's config file (see WaylandClient::host_config()).
    unsigned const workspace_rows = WaylandClient::the().host_config().workspace_rows;
    unsigned const workspace_columns = WaylandClient::the().host_config().workspace_columns;

    // Client-side HiDPI: rasterise at the compositor's output scale so windows are
    // drawn at native resolution on any compositor, with no SDE-specific protocol.
    // Fonts stay as they are (the fixed bitmap family); glyphs are scaled along
    // with everything else, keeping the UI in proportion. Set SDE_CLIENT_SCALE=0 to
    // force 1x (debugging).
    int client_scale = wayland.output_scale();
    if (auto const* env = getenv("SDE_CLIENT_SCALE"); env && *env && *env == '0')
        client_scale = 1;
    GUI::Window::set_backing_store_scale(client_scale);
    WaylandClient::the().set_client_scale(client_scale);

    // R4.7: the session font. Prefer the font broker (else the Katica/Csilla
    // bitmap defaults), so a client's first FontDatabase update already uses it.
    auto font_queries = read_fonts_broker();
    ByteString default_query = font_queries.default_query.is_empty() ? ByteString { "Katica 10 400 0" } : font_queries.default_query;
    ByteString fixed_query = font_queries.fixed_width_query.is_empty() ? ByteString { "Csilla 10 400 0" } : font_queries.fixed_width_query;
    ByteString title_query = font_queries.window_title_query.is_empty() ? ByteString { "Katica 10 700 0" } : font_queries.window_title_query;

    async_fast_greet(
        move(screen_rects),
        0, workspace_rows, workspace_columns,
        Gfx::current_system_theme_buffer(),
        default_query,
        fixed_query,
        title_query,
        move(effects),
        m_client_id);
}

void WindowServerConnection::send_paint(Window& window, Vector<Gfx::IntRect> rects)
{
    // Record what the repaint will change, so the present copies and damages only
    // those regions instead of the whole window.
    WaylandClient::the().add_window_damage(window.id, rects);
    async_paint(window.id, window.rect.size(), move(rects));
}

void WindowServerConnection::request_window_resize(i32 window_id, Gfx::IntSize size)
{
    auto* w = window(window_id);
    if (!w)
        return;

    // Keep at most one configure-driven repaint in flight; the next one is
    // sent when the client finishes this one (see set_window_backing_store).
    // A burst of compositor configures therefore collapses to one repaint per
    // client frame instead of one per configure.
    if (w->resize_in_flight) {
        w->pending_resize = size;
        w->has_pending_resize = true;
        return;
    }

    w->resize_in_flight = true;
    w->rect.set_size(size);
    dbgln("LibWM: window_resized {} -> {}x{}", window_id, size.width(), size.height());
    async_window_resized(window_id, w->rect);
    Vector<Gfx::IntRect> rects;
    rects.append({ 0, 0, size.width(), size.height() });
    send_paint(*w, move(rects));
}

void WindowServerConnection::flush_pending_resize(Window& w)
{
    w.resize_in_flight = false;
    if (!w.has_pending_resize)
        return;
    auto size = w.pending_resize;
    w.has_pending_resize = false;
    request_window_resize(w.id, size);
}

void WindowServerConnection::install_input_callbacks()
{
    WaylandClient::InputCallbacks callbacks;

    callbacks.mouse_move = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers) {
        async_mouse_move(window_id, position, 0, buttons, modifiers, 0, 0, 0, 0);
    };
    callbacks.mouse_down = [this](i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers) {
        // A press on one of our windows is outside any open menu (menu clicks are
        // routed to the popup), so dismiss it first.
        if (m_menu.has_open_menu())
            m_menu.close_open_menus();
        m_last_mouse_down_window_id = window_id;
        async_mouse_down(window_id, position, button, buttons, modifiers, 0, 0, 0, 0);
    };
    callbacks.mouse_up = [this](i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers) {
        async_mouse_up(window_id, position, button, buttons, modifiers, 0, 0, 0, 0);

        // Mirror WindowServer::WindowManager: a second press/release within the
        // double-click interval and distance is delivered to the client as a
        // MouseDoubleClick, after the MouseUp. GUI views use this for
        // double-click activation (e.g. opening a file in the file picker).
        if (window_id != m_double_click_window) {
            m_double_click_window = window_id;
            m_double_click_metadata.clear();
        }
        auto& metadata = m_double_click_metadata.ensure(button);
        auto delta = position - metadata.last_position;
        int distance_squared = delta.x() * delta.x() + delta.y() * delta.y();
        bool is_double_click = metadata.clock.is_valid()
            && metadata.clock.elapsed_milliseconds() < m_double_click_speed
            && distance_squared <= m_max_distance_for_double_click * m_max_distance_for_double_click;
        if (is_double_click) {
            dbgln("LibWM: mouse double-click window {} at {},{} button={}", window_id, position.x(), position.y(), button);
            async_mouse_double_click(window_id, position, button, buttons, modifiers, 0, 0, 0, 0);
            metadata.clock.reset();
        } else {
            metadata.clock.start();
        }
        metadata.last_position = position;
    };
    callbacks.mouse_wheel = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, i32 wheel_delta_x, i32 wheel_delta_y) {
        async_mouse_wheel(window_id, position, 0, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_x, wheel_delta_y);
    };
    callbacks.key = [this](i32 window_id, u32 code_point, u32 key, u8 map_entry_index, u32 modifiers, u32 scancode, bool is_press) {
        // While a menu is open (or to open one with F10/Alt+letter) the menu handles keys.
        if (m_menu.handle_key(window_id, key, code_point, modifiers, is_press))
            return;
        if (is_press)
            async_key_down(window_id, code_point, key, map_entry_index, modifiers, scancode);
        else
            async_key_up(window_id, code_point, key, map_entry_index, modifiers, scancode);
    };
    callbacks.window_entered = [this](i32 window_id) { async_window_entered(window_id); };
    callbacks.window_left = [this](i32 window_id) { async_window_left(window_id); };
    callbacks.window_close_request = [this](i32 window_id) { async_window_close_request(window_id); };
    callbacks.window_activation = [this](i32 window_id, bool activated) {
        if (activated) {
            m_active_window_id = window_id;
            async_window_activated(window_id);
        } else {
            if (m_active_window_id == window_id)
                m_active_window_id = -1;
            async_window_deactivated(window_id);
        }
    };
    callbacks.window_resize = [this](i32 window_id, Gfx::IntSize size) {
        request_window_resize(window_id, size);
    };
    callbacks.menubar_motion = [this](i32 window_id, Gfx::IntPoint position) { m_menu.on_menubar_motion(window_id, position); };
    callbacks.menubar_left = [this](i32 window_id) { m_menu.on_menubar_left(window_id); };
    callbacks.menubar_press = [this](i32 window_id, Gfx::IntPoint position) { m_menu.on_menubar_press(window_id, position); };
    callbacks.popup_motion = [this](i32 popup_id, Gfx::IntPoint position) { m_menu.on_popup_motion(popup_id, position); };
    callbacks.popup_button = [this](i32 popup_id, Gfx::IntPoint position, bool pressed) { m_menu.on_popup_button(popup_id, position, pressed); };
    callbacks.popup_closed = [this](i32 popup_id) { m_menu.on_popup_closed(popup_id); };
    callbacks.drag_finished = [this](bool accepted) {
        if (accepted)
            async_drag_accepted();
        else
            async_drag_cancelled();
    };
    callbacks.drag_moved = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, HashMap<String, ByteBuffer> mime_data) {
        async_drag_moved(window_id, position, 0, buttons, modifiers, {}, move(mime_data));
    };
    callbacks.drag_dropped = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, HashMap<String, ByteBuffer> mime_data) {
        async_drag_dropped(window_id, position, 0, buttons, modifiers, {}, move(mime_data));
    };

    WaylandClient::the().set_input_callbacks(move(callbacks));
}

void WindowServerConnection::install_menu_callbacks()
{
    m_menu.show_popup = [this](i32 menu_id, i32 window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu) {
        WaylandClient::the().create_popup(menu_id, window_id, parent_popup_id, anchor, size, is_submenu);
        present_menu_popup(menu_id);
    };
    m_menu.hide_popup = [](i32 menu_id) { WaylandClient::the().destroy_popup(menu_id); };
    m_menu.item_activated = [this](i32 menu_id, u32 identifier) {
        dbgln("LibWM: menu item activated menu={} id={}", menu_id, identifier);
        async_menu_item_activated(menu_id, identifier);
    };
    m_menu.item_entered = [this](i32 menu_id, u32 identifier) { async_menu_item_entered(menu_id, identifier); };
    m_menu.item_left = [this](i32 menu_id, u32 identifier) { async_menu_item_left(menu_id, identifier); };
    m_menu.visibility_changed = [this](i32 menu_id, bool visible) { async_menu_visibility_did_change(menu_id, visible); };
    m_menu.menubar_changed = [this](i32 window_id) { update_window_menubar(window_id); };
    m_menu.redraw_popup = [this](i32 menu_id) { present_menu_popup(menu_id); };
    WaylandClient::the().set_menubar_visibility_callback([this](i32 window_id, bool) { update_window_menubar(window_id); });
    WaylandClient::the().set_dismiss_menus_callback([this] { m_menu.close_open_menus(); });
    // The compositor places windows itself; adopt its position so the client's
    // screen-relative coordinates (menus, popups, tooltips) match reality.
    WaylandClient::the().set_toplevel_position_callback([this](i32 window_id, Gfx::IntPoint position) {
        if (auto* w = window(window_id))
            w->rect.set_location(position);
    });
}

void WindowServerConnection::present_menu_popup(i32 menu_id)
{
    auto size = m_menu.popup_size(menu_id);
    if (size.is_empty())
        return;
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, size);
    if (bitmap.is_error())
        return;
    Gfx::Painter painter(*bitmap.value());
    m_menu.render_popup(menu_id, painter);
    WaylandClient::the().present_popup(menu_id, *bitmap.value());
}

void WindowServerConnection::update_window_menubar(i32 window_id)
{
    // The compositor's window menu can hide the menu bar; the client still
    // decides whether it has one at all.
    bool present = m_menu.window_has_menubar(window_id);
    bool visible = WaylandClient::the().window_menubar_visible(window_id);
    int inset = (present && visible) ? m_menu.menubar_height() : 0;
    auto& wayland = WaylandClient::the();
    if (inset > 0) {
        wayland.set_window_inset(window_id, inset, [this, window_id](Gfx::Bitmap& bitmap, Gfx::IntRect rect) {
            Gfx::Painter painter(bitmap);
            m_menu.render_menubar(window_id, painter, rect);
        });
    } else {
        wayland.set_window_inset(window_id, 0, { });
    }
    if (auto* w = window(window_id)) {
        Vector<Gfx::IntRect> rects;
        rects.append({ 0, 0, w->rect.width(), w->rect.height() });
        send_paint(*w, move(rects));
    }
}

void WindowServerConnection::present(Window& window)
{
    if (!window.bitmap)
        return;

    if (auto const* path = getenv("LIBWM_DUMP")) {
        if (auto encoded = Gfx::PNGWriter::encode(*window.bitmap); !encoded.is_error()) {
            auto bytes = encoded.release_value();
            int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                (void)::write(fd, bytes.data(), bytes.size());
                ::close(fd);
            }
            dbgln("LibWM: dumped window {} ({}x{}) to {}", window.id, window.bitmap->width(), window.bitmap->height(), path);
        } else {
            dbgln("LibWM: PNG encode failed: {}", encoded.error());
        }
    }

    dbgln("LibWM: window {} presented ({}x{}, serial {}, alpha={})",
        window.id, window.bitmap->width(), window.bitmap->height(), window.last_serial, window.has_alpha_channel);
}

void WindowServerConnection::create_window(i32 window_id, i32, Gfx::IntRect const& rect, bool, bool has_alpha_channel, bool, bool, bool resizable, bool fullscreen, bool, bool, float, Gfx::IntSize, Gfx::IntSize, Gfx::IntSize minimum_size, Optional<Gfx::IntSize> const&, i32 window_type, i32 window_mode, ByteString const& title, i32 parent_window_id, Gfx::IntRect const& launch_origin_rect)
{
    auto window = make<Window>();
    window->id = window_id;
    window->rect = rect;
    window->minimum_size = minimum_size;
    window->has_alpha_channel = has_alpha_channel;
    window->title = title;
    window->window_mode = window_mode;
    window->parent_window_id = parent_window_id;

    // A window created fullscreen starts at the output size; the compositor
    // confirms it with a configure. Serenity's WindowServer sizes such windows
    // synchronously, and apps query window->size() right after show() (e.g. the
    // Tubes screensaver calls create_buffer(window->size())).
    //
    // The same applies to a window created with an empty size: the Desktop fills
    // the output via layer-shell anchors, but the app builds its backing store
    // from window->size() before the configure arrives, and an empty size trips
    // `VERIFY(!size.is_empty())`.
    if (fullscreen || window->rect.size().is_empty()) {
        auto screen = WaylandClient::the().screen_size();
        if (!screen.is_empty())
            window->rect.set_size(screen);
    }

    dbgln("LibWM: create_window id={} rect={},{},{}x{} fullscreen={} title='{}'", window_id, window->rect.x(), window->rect.y(), window->rect.width(), window->rect.height(), fullscreen, title);
    m_windows.append(move(window));

    auto& created_window = *m_windows.last();
    WaylandClient::the().create_window(window_id, created_window.rect.location(), created_window.rect.size(), title, has_alpha_channel, resizable, window_type, window_mode, parent_window_id, launch_origin_rect);

    // Ask the client to paint the whole window.
    Vector<Gfx::IntRect> rects;
    rects.append({ 0, 0, created_window.rect.width(), created_window.rect.height() });
    send_paint(created_window, move(rects));
}

void WindowServerConnection::set_window_title(i32 window_id, ByteString const& title)
{
    dbgln("LibWM: set_window_title id={} title='{}'", window_id, title);
    if (auto* w = window(window_id)) {
        w->title = title;
        WaylandClient::the().set_title(window_id, title);
    }
}

Messages::WindowServer::DestroyWindowResponse WindowServerConnection::destroy_window(i32 window_id)
{
    Vector<i32> destroyed;
    if (window(window_id)) {
        destroyed.append(window_id);
        WaylandClient::the().destroy_window(window_id);
        remove_window(window_id);
    }
    return Messages::WindowServer::DestroyWindowResponse { move(destroyed) };
}

Messages::WindowServer::SetWindowRectResponse WindowServerConnection::set_window_rect(i32 window_id, Gfx::IntRect const& rect)
{
    if (auto* w = window(window_id)) {
        w->rect.set_size(rect.size());
        w->rect.set_location(rect.location());
    }
    // Layer-surface windows (the panel) derive their size from this.
    WaylandClient::the().set_window_rect(window_id, rect.size());
    return Messages::WindowServer::SetWindowRectResponse { rect };
}

Messages::WindowServer::GetWindowRectResponse WindowServerConnection::get_window_rect(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowRectResponse { w->rect };
    return Messages::WindowServer::GetWindowRectResponse { Gfx::IntRect { } };
}

Messages::WindowServer::GetWindowFloatingRectResponse WindowServerConnection::get_window_floating_rect(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowFloatingRectResponse { w->rect };
    return Messages::WindowServer::GetWindowFloatingRectResponse { Gfx::IntRect { } };
}

void WindowServerConnection::set_window_minimum_size(i32 window_id, Gfx::IntSize size)
{
    if (auto* w = window(window_id))
        w->minimum_size = size;
}

Messages::WindowServer::GetWindowMinimumSizeResponse WindowServerConnection::get_window_minimum_size(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowMinimumSizeResponse { w->minimum_size };
    return Messages::WindowServer::GetWindowMinimumSizeResponse { Gfx::IntSize { } };
}

Messages::WindowServer::GetWindowTitleResponse WindowServerConnection::get_window_title(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowTitleResponse { w->title };
    return Messages::WindowServer::GetWindowTitleResponse { ByteString { } };
}

Messages::WindowServer::IsWindowModifiedResponse WindowServerConnection::is_window_modified(i32)
{
    return Messages::WindowServer::IsWindowModifiedResponse { false };
}

Messages::WindowServer::GetAppletRectOnScreenResponse WindowServerConnection::get_applet_rect_on_screen(i32)
{
    return Messages::WindowServer::GetAppletRectOnScreenResponse { Gfx::IntRect { } };
}

Messages::WindowServer::GetWindowRectFromClientResponse WindowServerConnection::get_window_rect_from_client(i32, i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowRectFromClientResponse { w->rect };
    return Messages::WindowServer::GetWindowRectFromClientResponse { Gfx::IntRect { } };
}

void WindowServerConnection::set_window_backing_store(i32 window_id, i32, i32 pitch, IPC::File const& anon_file, i32 serial, bool has_alpha_channel, Gfx::IntSize size, Gfx::IntSize visible_size, bool)
{
    auto* w = window(window_id);
    if (!w) {
        dbgln("LibWM: backing store for unknown window {}", window_id);
        return;
    }

    // The client has finished a paint (this store), so it's ready for the next
    // size: release any resize we coalesced while it was busy.
    flush_pending_resize(*w);

    if (w->bitmap && w->last_serial == serial) {
        present(*w);
        return;
    }

    if (size.is_empty())
        return;

    auto format = has_alpha_channel ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    size_t bytes = static_cast<size_t>(pitch) * static_cast<size_t>(size.height());

    int dupfd = ::dup(anon_file.fd());
    if (dupfd < 0)
        return;

    auto buffer = Core::AnonymousBuffer::create_from_anon_fd(dupfd, bytes);
    if (buffer.is_error()) {
        dbgln("LibWM: failed to map backing store: {}", buffer.error());
        return;
    }

    auto bitmap = Gfx::Bitmap::create_with_anonymous_buffer(format, buffer.release_value(), size, 1);
    if (bitmap.is_error()) {
        dbgln("LibWM: failed to create bitmap: {}", bitmap.error());
        return;
    }

    w->bitmap = bitmap.release_value();
    w->has_alpha_channel = has_alpha_channel;
    w->last_serial = serial;
    present(*w);
    WaylandClient::the().attach_and_commit(window_id, anon_file.fd(), size, visible_size, pitch, has_alpha_channel);
}

void WindowServerConnection::invalidate_rect(i32 window_id, Vector<Gfx::IntRect> const& rects, bool)
{
    auto* w = window(window_id);
    if (!w)
        return;

    Vector<Gfx::IntRect> copy;
    copy.ensure_capacity(rects.size());
    for (auto const& rect : rects)
        copy.append(rect);
    if (copy.is_empty())
        copy.append({ 0, 0, w->rect.width(), w->rect.height() });
    send_paint(*w, move(copy));
}

void WindowServerConnection::did_finish_painting(i32, Vector<Gfx::IntRect> const&)
{
}

void WindowServerConnection::set_window_has_alpha_channel(i32 window_id, bool has_alpha_channel)
{
    if (auto* w = window(window_id))
        w->has_alpha_channel = has_alpha_channel;
}

void WindowServerConnection::set_window_alpha_hit_threshold(i32, float)
{
}

void WindowServerConnection::set_window_icon_bitmap(i32 window_id, Gfx::ShareableBitmap const& icon)
{
    WaylandClient::the().set_window_icon_bitmap(window_id, icon);
}

void WindowServerConnection::set_window_progress(i32, Optional<i32> const&)
{
}

void WindowServerConnection::set_fullscreen(i32 window_id, bool fullscreen)
{
    dbgln("LibWM: set_fullscreen({}, {})", window_id, fullscreen);
    WaylandClient::the().set_fullscreen(window_id, fullscreen);
}

void WindowServerConnection::set_maximized(i32 window_id, bool maximized)
{
    WaylandClient::the().set_maximized(window_id, maximized);
}

void WindowServerConnection::set_minimized(i32 window_id, bool minimized)
{
    if (minimized)
        WaylandClient::the().set_minimized(window_id);
}

void WindowServerConnection::move_window_to_front(i32)
{
}

Messages::WindowServer::GetGlobalCursorPositionResponse WindowServerConnection::get_global_cursor_position()
{
    // The compositor owns window placement, so the client reconstructs the
    // cursor's output position from the surface under the pointer and that
    // surface's known origin (see WaylandClient::global_pointer_position).
    return Messages::WindowServer::GetGlobalCursorPositionResponse { WaylandClient::the().global_pointer_position() };
}

Messages::WindowServer::GetColorUnderCursorResponse WindowServerConnection::get_color_under_cursor()
{
    return Messages::WindowServer::GetColorUnderCursorResponse { Optional<Gfx::Color> { } };
}

Messages::WindowServer::GetWallpaperResponse WindowServerConnection::get_wallpaper()
{
    if (m_wallpaper)
        return m_wallpaper->to_shareable_bitmap();
    // The desktop owns the wallpaper; the compositor sends it over serenity-window.
    if (auto* host = WaylandClient::the().host_wallpaper()) {
        m_wallpaper = host;
        return m_wallpaper->to_shareable_bitmap();
    }
    return Gfx::ShareableBitmap { };
}

Messages::WindowServer::SetWallpaperResponse WindowServerConnection::set_wallpaper(Gfx::ShareableBitmap const& wallpaper_bitmap)
{
    m_wallpaper = wallpaper_bitmap.bitmap();
    // The desktop owns the wallpaper: hand the raw pixels over (no PNG encode,
    // which would stall this application's UI thread).
    WaylandClient::the().set_host_wallpaper(m_wallpaper.ptr());
    return true;
}

void WindowServerConnection::set_wallpaper_mode(ByteString const& mode)
{
    m_wallpaper_mode = mode;
    WaylandClient::the().set_host_setting("wallpaper.mode"sv, mode);
}

void WindowServerConnection::set_background_color(ByteString const& background_color)
{
    WaylandClient::the().set_host_setting("wallpaper.color"sv, background_color);
}

Messages::WindowServer::StartDragResponse WindowServerConnection::start_drag(ByteString const& text, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& drag_bitmap)
{
    // Bridge LibGUI's drag source onto the standard Wayland data-device DnD. The
    // outcome arrives later, via callbacks.drag_finished (async_drag_accepted /
    // async_drag_cancelled), so a Serenity app's DragOperation resolves when the
    // drag ends — against any Wayland target, SDE or otherwise.
    bool started = WaylandClient::the().start_drag(text, mime_data, drag_bitmap);
    return Messages::WindowServer::StartDragResponse { started };
}

void WindowServerConnection::set_accepts_drag(bool accepts)
{
    WaylandClient::the().set_accepts_drag(accepts);
}

void WindowServerConnection::create_menu(i32 menu_id, String const& name, i32 minimum_width)
{
    m_menu.create_menu(menu_id, name.to_byte_string(), minimum_width);
}

void WindowServerConnection::set_menu_name(i32 menu_id, String const& name)
{
    m_menu.set_menu_name(menu_id, name.to_byte_string());
}

void WindowServerConnection::set_menu_minimum_width(i32 menu_id, i32 minimum_width)
{
    m_menu.set_menu_minimum_width(menu_id, minimum_width);
}

void WindowServerConnection::destroy_menu(i32 menu_id)
{
    m_menu.destroy_menu(menu_id);
}

void WindowServerConnection::add_menu(i32 window_id, i32 menu_id)
{
    m_menu.add_menu(window_id, menu_id);
}

void WindowServerConnection::add_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, Gfx::ShareableBitmap const& icon, bool exclusive)
{
    m_menu.add_menu_item(menu_id, identifier, submenu_id, text, enabled, visible, checkable, checked, is_default, shortcut, icon.is_valid() ? icon.bitmap() : nullptr, exclusive);
}

void WindowServerConnection::add_menu_separator(i32 menu_id)
{
    m_menu.add_menu_separator(menu_id);
}

void WindowServerConnection::update_menu_item(i32 menu_id, i32 identifier, i32 submenu_id, ByteString const& text, bool enabled, bool visible, bool checkable, bool checked, bool is_default, ByteString const& shortcut, Gfx::ShareableBitmap const& icon)
{
    m_menu.update_menu_item(menu_id, identifier, submenu_id, text, enabled, visible, checkable, checked, is_default, shortcut, icon.is_valid() ? icon.bitmap() : nullptr);
}

void WindowServerConnection::remove_menu_item(i32 menu_id, i32 identifier)
{
    m_menu.remove_menu_item(menu_id, identifier);
}

void WindowServerConnection::flash_menubar_menu(i32, i32)
{
}

void WindowServerConnection::popup_menu(i32 menu_id, Gfx::IntPoint screen_position, Gfx::IntRect const& button_rect)
{
    // For menubar menus the owner window is known; context menus use the active
    // window as the xdg_popup parent. Wayland clients don't know global screen
    // coordinates, so positions are treated as parent-surface-relative.
    i32 parent = m_menu.menu_window(menu_id);
    if (parent < 0)
        parent = m_last_mouse_down_window_id;
    if (parent < 0)
        parent = m_active_window_id;
    if (parent < 0)
        return;
    // The client passes the button's screen position plus its (local) rect; the
    // menu is placed from the screen position (the rect is only its size).
    auto anchor = button_rect.is_empty()
        ? Gfx::IntRect { screen_position, { 1, 1 } }
        : Gfx::IntRect { screen_position, button_rect.size() };
    m_menu.open_root(menu_id, parent, anchor);
}

void WindowServerConnection::dismiss_menu(i32 menu_id)
{
    m_menu.close_menu(menu_id);
}

namespace {
ByteString theme_broker_path()
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-theme", runtime);
    return ByteString { "/tmp/sde-theme" };
}

ByteString fonts_broker_path()
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-fonts", runtime);
    return ByteString { "/tmp/sde-fonts" };
}

FontQueries read_fonts_broker()
{
    FontQueries queries;
    auto file = Core::File::open(fonts_broker_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return queries;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return queries;
    auto lines = StringView { contents.value().bytes() }.split_view('\n');
    if (lines.size() > 0)
        queries.default_query = lines[0].trim_whitespace().to_byte_string();
    if (lines.size() > 1)
        queries.fixed_width_query = lines[1].trim_whitespace().to_byte_string();
    if (lines.size() > 2)
        queries.window_title_query = lines[2].trim_whitespace().to_byte_string();
    return queries;
}

void write_fonts_broker(ByteString const& default_query, ByteString const& fixed_width_query, ByteString const& window_title_query)
{
    if (auto file = Core::File::open(fonts_broker_path(), Core::File::OpenMode::Write); !file.is_error())
        (void)file.value()->write_until_depleted(ByteString::formatted("{}\n{}\n{}\n", default_query, fixed_width_query, window_title_query));
}

ByteString resource_root_path()
{
    if (auto const* res = getenv("SERENITY_RES_ROOT"); res && *res)
        return ByteString(res);
    return {};
}

ByteString host_path_for_resource(ByteString const& path)
{
    if (!path.starts_with("/res/"sv))
        return path;
    auto root = resource_root_path();
    if (root.is_empty())
        return path;
    return ByteString::formatted("{}/{}", root, path.substring_view(5));
}
}

Messages::WindowServer::GetSystemThemeResponse WindowServerConnection::get_system_theme()
{
    return m_theme_name;
}

Messages::WindowServer::SetSystemThemeResponse WindowServerConnection::set_system_theme(ByteString const& theme_path, ByteString const& theme_name, bool, Optional<ByteString> const& color_scheme_path)
{
    auto host_theme = host_path_for_resource(theme_path);
    Optional<ByteString> host_scheme;
    if (color_scheme_path.has_value())
        host_scheme = host_path_for_resource(color_scheme_path.value());

    auto buffer = Gfx::load_system_theme(host_theme, host_scheme);
    if (buffer.is_error())
        return false;

    m_theme_name = theme_name;
    // Restyle this client, then record the choice so other clients and the
    // compositor pick it up from the shared broker file.
    async_update_system_theme(buffer.value());
    if (auto file = Core::File::open(theme_broker_path(), Core::File::OpenMode::Write); !file.is_error())
        (void)file.value()->write_until_depleted(host_theme);
    // Persist the choice so it survives a restart (the compositor applies [Theme]
    // at startup).
    WaylandClient::the().set_host_setting("theme.name"sv, theme_name);
    WaylandClient::the().set_host_setting("theme.path"sv, host_theme);
    return true;
}

void WindowServerConnection::watch_theme()
{
    if (m_theme_watcher)
        return;
    // Apply any already-chosen theme immediately (a client started after a theme
    // change, or a fresh session with a persisted [Theme], must pick it up), then
    // follow further changes.
    apply_theme_from_broker();
    m_theme_watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    m_theme_watcher->on_change = [this](Core::FileWatcherEvent const& event) {
        if (event.event_path != theme_broker_path())
            return;
        apply_theme_from_broker();
    };
    // Watch the runtime directory so creation of the file is seen too, and the
    // file itself (a write is IN_MODIFY, which a directory watch does not report).
    if (auto directory = LexicalPath(theme_broker_path()).dirname(); !directory.is_empty())
        (void)m_theme_watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    auto path = theme_broker_path();
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)m_theme_watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

void WindowServerConnection::apply_theme_from_broker()
{
    ByteString host_theme;
    if (auto file = Core::File::open(theme_broker_path(), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error())
            host_theme = StringView { contents.value() }.trim_whitespace().to_byte_string();
    }
    // Fall back to the persisted theme ([Theme]) when there is no live broker value.
    if (host_theme.is_empty())
        host_theme = WaylandClient::the().host_setting("theme.path"sv, ""sv);
    if (host_theme.is_empty())
        return;
    if (auto buffer = Gfx::load_system_theme(host_theme, {}); !buffer.is_error()) {
        auto name = WaylandClient::the().host_setting("theme.name"sv, ""sv);
        m_theme_name = name.is_empty() ? ByteString { LexicalPath(host_theme).title() } : name;
        async_update_system_theme(buffer.value());
    }
}

void WindowServerConnection::watch_fonts()
{
    if (m_fonts_watcher)
        return;
    // Apply any already-chosen font immediately (a client started after a font
    // change must still pick it up), then follow further changes.
    apply_fonts_from_broker();
    m_fonts_watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    m_fonts_watcher->on_change = [this](Core::FileWatcherEvent const& event) {
        if (event.event_path != fonts_broker_path())
            return;
        apply_fonts_from_broker();
    };
    if (auto directory = LexicalPath(fonts_broker_path()).dirname(); !directory.is_empty())
        (void)m_fonts_watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    auto path = fonts_broker_path();
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)m_fonts_watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

void WindowServerConnection::apply_fonts_from_broker()
{
    auto queries = read_fonts_broker();
    if (queries.default_query.is_empty())
        return;
    dbgln("LibWM: applying font broker (default '{}')", queries.default_query);
    // The connection runs in-process with the client, so set the queries directly
    // too: widgets created afterwards (e.g. Display Settings' Fonts tab) then see
    // the chosen font, not just those relayouted by the async notification.
    Gfx::FontDatabase::set_default_font_query(queries.default_query);
    if (!queries.fixed_width_query.is_empty())
        Gfx::FontDatabase::set_fixed_width_font_query(queries.fixed_width_query);
    if (!queries.window_title_query.is_empty())
        Gfx::FontDatabase::set_window_title_font_query(queries.window_title_query);
    async_update_system_fonts(queries.default_query, queries.fixed_width_query, queries.window_title_query);
}

Messages::WindowServer::IsSystemThemeOverriddenResponse WindowServerConnection::is_system_theme_overridden()
{
    return false;
}

Messages::WindowServer::GetPreferredColorSchemeResponse WindowServerConnection::get_preferred_color_scheme()
{
    return ByteString { "Default" };
}

Messages::WindowServer::GetScreenLayoutResponse WindowServerConnection::get_screen_layout()
{
    // R4.5: the desktop owns the output layout; it is served over serenity-window
    // as "screenlayout" (main=<i>;<Mode>,<Device>,<L>,<T>,<W>,<H>,<Scale>;...).
    auto encoded = WaylandClient::the().host_setting("screenlayout"sv);
    if (!encoded.is_empty()) {
        WindowServer::ScreenLayout parsed;
        for (auto field : encoded.split_view(';')) {
            if (field.starts_with("main="sv)) {
                parsed.main_screen_index = field.substring_view(5).to_number<unsigned>().value_or(0);
                continue;
            }
            auto parts = field.split_view(',');
            if (parts.size() < 7)
                continue;
            auto mode = WindowServer::ScreenLayout::Screen::Mode::Invalid;
            if (parts[0] == "Device"sv)
                mode = WindowServer::ScreenLayout::Screen::Mode::Device;
            else if (parts[0] == "Virtual"sv)
                mode = WindowServer::ScreenLayout::Screen::Mode::Virtual;
            if (mode == WindowServer::ScreenLayout::Screen::Mode::Invalid)
                continue;
            Optional<ByteString> device;
            if (mode == WindowServer::ScreenLayout::Screen::Mode::Device && !parts[1].is_empty())
                device = parts[1].to_byte_string();
            parsed.screens.append(WindowServer::ScreenLayout::Screen {
                .mode = mode,
                .device = move(device),
                .location = { parts[2].to_number<int>().value_or(0), parts[3].to_number<int>().value_or(0) },
                .resolution = { parts[4].to_number<int>().value_or(0), parts[5].to_number<int>().value_or(0) },
                .scale_factor = parts[6].to_number<int>().value_or(1),
            });
        }
        if (!parsed.screens.is_empty())
            return parsed;
    }

    WindowServer::ScreenLayout layout;
    WindowServer::ScreenLayout::Screen screen {
        .mode = WindowServer::ScreenLayout::Screen::Mode::Device,
        .device = ByteString { "sde-0" },
        .location = { 0, 0 },
        .resolution = m_screen_size,
        .scale_factor = 1,
    };
    layout.screens.append(move(screen));
    layout.main_screen_index = 0;
    return layout;
}

Messages::WindowServer::SetScreenLayoutResponse WindowServerConnection::set_screen_layout(WindowServer::ScreenLayout const& screen_layout, bool save)
{
    // R4.5: the desktop owns the output layout, so hand it over (the compositor
    // applies and persists it) rather than writing its screenlayout.ini.
    (void)save;
    StringBuilder encoded;
    encoded.appendff("main={}", screen_layout.main_screen_index);
    for (auto const& screen : screen_layout.screens)
        encoded.appendff(";{},{},{},{},{},{},{}", WindowServer::ScreenLayout::Screen::mode_to_string(screen.mode),
            screen.device.value_or(ByteString {}), screen.location.x(), screen.location.y(),
            screen.resolution.width(), screen.resolution.height(), screen.scale_factor);
    WaylandClient::the().set_host_setting("screenlayout"sv, encoded.string_view());
    return { true, {} };
}

Messages::WindowServer::SaveScreenLayoutResponse WindowServerConnection::save_screen_layout()
{
    return { true, {} };
}

void WindowServerConnection::set_output_scale(i32 scale_v120)
{
    // R6 HiDPI: the DisplaySettings scale control calls this; we write the
    // compositor's [Output] DefaultScale, which it live-applies (see
    // docs/14-r4.4-config.md). v120 carries a fractional scale exactly (1.5 -> 180).
    if (scale_v120 < 60) // ignore nonsense (< 0.5x)
        return;
    double const scale = static_cast<double>(scale_v120) / 120.0;
    // The desktop owns the output scale; hand it over (the compositor applies and
    // persists it) rather than writing its config.
    WaylandClient::the().set_host_setting("output.default_scale"sv, ByteString::formatted("{}", scale));
    dbgln("LibWM: set output scale {}x", scale);
}

// --- mouse/pointer settings (R4.6) -----------------------------------------

void WindowServerConnection::set_mouse_acceleration(float factor)
{
    WaylandClient::the().set_host_setting("mouse.acceleration"sv, ByteString::formatted("{}", factor));
}

Messages::WindowServer::GetMouseAccelerationResponse WindowServerConnection::get_mouse_acceleration()
{
    return WaylandClient::the().host_setting("mouse.acceleration"sv, "1.0"sv).to_number<double>().value_or(1.0);
}

void WindowServerConnection::set_scroll_step_size(u32 step_size)
{
    WaylandClient::the().set_host_setting("mouse.scroll_step_size"sv, ByteString::number(step_size));
}

Messages::WindowServer::GetScrollStepSizeResponse WindowServerConnection::get_scroll_step_size()
{
    return WaylandClient::the().host_setting("mouse.scroll_step_size"sv, "1"sv).to_number<u32>().value_or(1);
}

void WindowServerConnection::set_double_click_speed(int speed)
{
    m_double_click_speed = speed;
    WaylandClient::the().set_host_setting("mouse.double_click_speed"sv, ByteString::number(speed));
}

Messages::WindowServer::GetDoubleClickSpeedResponse WindowServerConnection::get_double_click_speed()
{
    m_double_click_speed = WaylandClient::the().host_setting("mouse.double_click_speed"sv, "250"sv).to_number<int>().value_or(250);
    return m_double_click_speed;
}

void WindowServerConnection::set_mouse_buttons_switched(bool switched)
{
    WaylandClient::the().set_host_setting("mouse.buttons_switched"sv, switched ? "true"sv : "false"sv);
}

Messages::WindowServer::AreMouseButtonsSwitchedResponse WindowServerConnection::are_mouse_buttons_switched()
{
    return WaylandClient::the().host_setting("mouse.buttons_switched"sv, "false"sv) == "true"sv;
}

void WindowServerConnection::set_natural_scroll(bool inverted)
{
    WaylandClient::the().set_host_setting("mouse.natural_scroll"sv, inverted ? "true"sv : "false"sv);
}

Messages::WindowServer::IsNaturalScrollResponse WindowServerConnection::is_natural_scroll()
{
    return WaylandClient::the().host_setting("mouse.natural_scroll"sv, "false"sv) == "true"sv;
}

Messages::WindowServer::GetMouseAccelerationMinResponse WindowServerConnection::get_mouse_acceleration_min()
{
    return static_cast<float>(WaylandClient::the().host_config().accel_min);
}

Messages::WindowServer::GetMouseAccelerationMaxResponse WindowServerConnection::get_mouse_acceleration_max()
{
    return static_cast<float>(WaylandClient::the().host_config().accel_max);
}

Messages::WindowServer::GetScrollStepSizeMinResponse WindowServerConnection::get_scroll_step_size_min()
{
    return WaylandClient::the().host_config().scroll_step_min;
}

Messages::WindowServer::GetDoubleClickSpeedMinResponse WindowServerConnection::get_double_click_speed_min()
{
    return WaylandClient::the().host_config().double_click_min;
}

Messages::WindowServer::GetDoubleClickSpeedMaxResponse WindowServerConnection::get_double_click_speed_max()
{
    return WaylandClient::the().host_config().double_click_max;
}

Messages::WindowServer::GetCursorThemeResponse WindowServerConnection::get_cursor_theme()
{
    return WaylandClient::the().host_setting("cursor.theme"sv, "Default"sv);
}

void WindowServerConnection::apply_cursor_theme(ByteString const& name)
{
    WaylandClient::the().set_host_setting("cursor.theme"sv, name);
}

Messages::WindowServer::GetCursorHighlightRadiusResponse WindowServerConnection::get_cursor_highlight_radius()
{
    return WaylandClient::the().host_setting("cursor.highlight_radius"sv, "0"sv).to_number<int>().value_or(0);
}

void WindowServerConnection::set_cursor_highlight_radius(int radius)
{
    WaylandClient::the().set_host_setting("cursor.highlight_radius"sv, ByteString::number(radius));
}

Messages::WindowServer::GetCursorHighlightColorResponse WindowServerConnection::get_cursor_highlight_color()
{
    if (auto color = Gfx::Color::from_string(WaylandClient::the().host_setting("cursor.highlight_color"sv, "#000000"sv)); color.has_value())
        return color.value();
    return Gfx::Color::Black;
}

void WindowServerConnection::set_cursor_highlight_color(Gfx::Color color)
{
    WaylandClient::the().set_host_setting("cursor.highlight_color"sv, color.to_byte_string());
}

Messages::WindowServer::GetKeyboardLayoutResponse WindowServerConnection::get_keyboard_layout()
{
    return { WaylandClient::the().host_setting("keyboard.layouts"sv, "us"sv),
        WaylandClient::the().host_setting("keyboard.variant"sv, ""sv),
        WaylandClient::the().host_setting("keyboard.model"sv, "pc105"sv),
        WaylandClient::the().host_setting("keyboard.options"sv, ""sv) };
}

void WindowServerConnection::set_keyboard_layout(ByteString const& layouts, ByteString const& variant, ByteString const& model, ByteString const& options)
{
    WaylandClient::the().set_host_setting("keyboard.layouts"sv, layouts);
    WaylandClient::the().set_host_setting("keyboard.variant"sv, variant);
    WaylandClient::the().set_host_setting("keyboard.model"sv, model);
    WaylandClient::the().set_host_setting("keyboard.options"sv, options);
}

Messages::WindowServer::GetKeyboardLayoutNamesResponse WindowServerConnection::get_keyboard_layout_names()
{
    Vector<ByteString> names;
    // Prefer the canonical xkb list: the [! layout] section of evdev.lst.
    if (auto file = Core::File::open("/usr/share/X11/xkb/rules/evdev.lst"sv, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            bool in_layout = false;
            StringView text { contents.value().bytes() };
            for (auto line : text.split_view('\n')) {
                if (line.starts_with("! "sv)) {
                    in_layout = line.starts_with("! layout"sv);
                    continue;
                }
                if (!in_layout)
                    continue;
                auto trimmed = line.trim_whitespace(TrimMode::Left);
                if (trimmed.is_empty())
                    continue;
                names.append(trimmed.split_view(' ').first());
            }
        }
    }
    if (names.is_empty()) {
        (void)Core::Directory::for_each_entry("/usr/share/X11/xkb/symbols"sv, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto&) -> ErrorOr<IterationDecision> {
            if (!entry.name.starts_with("_"sv))
                names.append(entry.name);
            return IterationDecision::Continue;
        });
    }
    names.append("us"sv);
    quick_sort(names);
    Vector<ByteString> unique;
    for (auto& name : names) {
        if (unique.is_empty() || unique.last() != name)
            unique.append(move(name));
    }
    return unique;
}

void WindowServerConnection::set_system_effects(Vector<bool> const& effects, u8 geometry, u8 tile_window)
{
    // Apply to this client (LibGUI's SystemEffects) and record the choices so the
    // compositor (geometry HUD) and other clients pick them up (R4.7).
    async_update_system_effects(effects);
    WaylandClient::the().set_host_setting("effects.show_geometry"sv, WindowServer::ShowGeometryTools::enum_to_string(static_cast<WindowServer::ShowGeometry>(geometry)));
    WaylandClient::the().set_host_setting("effects.tile_window"sv, WindowServer::TileWindowTools::enum_to_string(static_cast<WindowServer::TileWindow>(tile_window)));
    static constexpr Array effect_names = {
        "AnimateMenus"sv, "FlashMenus"sv, "AnimateWindows"sv, "SmoothScrolling"sv,
        "TabAccents"sv, "SplitterKnurls"sv, "Tooltips"sv, "MenuShadow"sv,
        "WindowShadow"sv, "TooltipShadow"sv,
    };
    for (size_t i = 0; i < effects.size() && i < effect_names.size(); ++i)
        WaylandClient::the().set_host_setting(ByteString::formatted("effects.{}", effect_names[i]), effects[i] ? "true"sv : "false"sv);
}

Messages::WindowServer::SetSystemFontsResponse WindowServerConnection::set_system_fonts(ByteString const& default_font_query, ByteString const& fixed_width_font_query, ByteString const& window_title_font_query)
{
    // Apply in-process (so subsequent lookups resolve the new font), restyle this
    // client and tell it to re-layout, then record the choice in the session font
    // broker so the compositor and other clients pick it up (R4.7).
    Gfx::FontDatabase::set_default_font_query(default_font_query);
    if (!fixed_width_font_query.is_empty())
        Gfx::FontDatabase::set_fixed_width_font_query(fixed_width_font_query);
    if (!window_title_font_query.is_empty())
        Gfx::FontDatabase::set_window_title_font_query(window_title_font_query);
    async_update_system_fonts(default_font_query, fixed_width_font_query, window_title_font_query);
    write_fonts_broker(default_font_query, fixed_width_font_query, window_title_font_query);
    return true;
}

Messages::WindowServer::ApplyWorkspaceSettingsResponse WindowServerConnection::apply_workspace_settings(u32 rows, u32 columns, bool save)
{
    // Write the grid to the session config; the compositor live-reloads it and the
    // workspace picker follows (R4.8). `save` is implied: the file is the store.
    (void)save;
    if (rows < 1 || columns < 1)
        return false;
    WaylandClient::the().set_host_setting("workspaces.rows"sv, ByteString::number(rows));
    WaylandClient::the().set_host_setting("workspaces.columns"sv, ByteString::number(columns));
    return true;
}

Messages::WindowServer::GetSystemThemeOverrideResponse WindowServerConnection::get_system_theme_override()
{
    // The port has no theme override (set_system_theme_override); report none.
    // Must be a valid response: the theme-change handler calls this synchronously.
    return Optional<Core::AnonymousBuffer> { };
}

}
