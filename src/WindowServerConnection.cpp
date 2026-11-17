/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowServerConnection.h"
#include "WaylandClient.h"
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGUI/Window.h>
#include <WindowServer/SystemEffects.h>
#include <WindowServer/WindowType.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (auto const* env = getenv("SDE_CLIENT_SCALE"); env && *env) {
        // A positive value selects a scale explicitly (useful for testing); 0 forces 1x.
        auto requested = atoi(env);
        client_scale = requested > 0 ? requested : 1;
    }
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
    // The damage is recorded from the rects the client actually paints (see
    // did_finish_painting), not from this request: the client folds in update()
    // rects that arrived in the meantime, so the invalidation set can be smaller
    // than the painted set.
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
    callbacks.popup_wheel = [this](i32 popup_id, i32 wheel_delta_y) { m_menu.on_popup_wheel(popup_id, wheel_delta_y); };
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
    m_menu.show_popup = [this](i32 menu_id, i32 window_id, i32 parent_popup_id, Gfx::IntRect anchor, Gfx::IntSize size, bool is_submenu, bool from_menubar) {
        // A menubar menu's rect is menubar-local. With the direct path the menubar is a subsurface above the
        // window-content origin, so move the anchor above the content top; the fallback path composes the
        // menubar into the content, where the rect is already in the right coordinate space.
        if (from_menubar && WaylandClient::the().window_uses_menubar_subsurface(window_id))
            anchor.translate_by(0, -m_menu.menubar_height());
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
        wayland.set_window_inset(window_id, 0, {});
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
                auto const written = ::write(fd, bytes.data(), bytes.size());
                if (written < 0)
                    dbgln("LibWM: failed to write dump {}: {}", path, strerror(errno));
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

void WindowServerConnection::create_window(i32 window_id, i32, Gfx::IntRect const& rect, bool, bool has_alpha_channel, bool, bool, bool resizable, bool fullscreen, bool frameless, bool, float, Gfx::IntSize, Gfx::IntSize, Gfx::IntSize minimum_size, Optional<Gfx::IntSize> const&, i32 window_type, i32 window_mode, ByteString const& title, i32 parent_window_id, Gfx::IntRect const& launch_origin_rect)
{
    auto window = make<Window>();
    window->id = window_id;
    window->rect = rect;
    window->minimum_size = minimum_size;
    window->has_alpha_channel = has_alpha_channel;
    window->title = title;
    window->window_mode = window_mode;
    window->parent_window_id = parent_window_id;
    window->client_positioned = window_type == static_cast<i32>(WindowServer::WindowType::Popup)
        || window_type == static_cast<i32>(WindowServer::WindowType::Autocomplete)
        || window_type == static_cast<i32>(WindowServer::WindowType::Tooltip)
        || window_type == static_cast<i32>(WindowServer::WindowType::Menu)
        || window_type == static_cast<i32>(WindowServer::WindowType::Notification);

    // A window created fullscreen starts at the output size; the compositor
    // confirms it with a configure. Serenity's WindowServer sizes such windows
    // synchronously, and apps query window->size() right after show() (e.g. the
    // Tubes screensaver calls create_buffer(window->size())).
    //
    // The same applies to a window created with an empty size: the Desktop fills
    // the output via layer-shell anchors, but the app builds its backing store
    // from window->size() before the configure arrives, and an empty size trips
    // `VERIFY(!size.is_empty())`.
    //
    // Only a window that will fill the output should assume the screen size,
    // though. A panel or applet is laid out by the compositor from its applet
    // area, so a screen-sized placeholder is allocated and thrown away: the Audio
    // applet (the only one that shows an unsized window) allocated three
    // 1280x720 buffers before its 16x16 configure — ~10 MB of memfd
    // (docs/25-performance.md §4).
    if (fullscreen || window->rect.size().is_empty()) {
        auto screen = WaylandClient::the().screen_size();
        if (!screen.is_empty()) {
            bool const compositor_sized = window_type == static_cast<i32>(WindowServer::WindowType::Applet)
                || window_type == static_cast<i32>(WindowServer::WindowType::Taskbar);
            window->rect.set_size(compositor_sized ? Gfx::IntSize { 16, 16 } : screen);
        }
    }

    dbgln("LibWM: create_window id={} rect={},{},{}x{} fullscreen={} title='{}'", window_id, window->rect.x(), window->rect.y(), window->rect.width(), window->rect.height(), fullscreen, title);
    m_windows.append(move(window));

    auto& created_window = *m_windows.last();
    WaylandClient::the().create_window(window_id, created_window.rect.location(), created_window.rect.size(), title, has_alpha_channel, resizable, frameless, window_type, window_mode, parent_window_id, launch_origin_rect);

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
        // The compositor owns a toplevel's/layer surface's placement. Serenity's
        // client still calls set_rect() (and reports its own idea of the origin),
        // but adopting that here would undo the position the compositor reported
        // and make screen-relative coordinates (ComboBox popups, menus, tooltips)
        // fall back to window-local ones. Only client-positioned popups follow it.
        if (w->client_positioned)
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
    return Messages::WindowServer::GetWindowRectResponse { Gfx::IntRect {} };
}

Messages::WindowServer::GetWindowFloatingRectResponse WindowServerConnection::get_window_floating_rect(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowFloatingRectResponse { w->rect };
    return Messages::WindowServer::GetWindowFloatingRectResponse { Gfx::IntRect {} };
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
    return Messages::WindowServer::GetWindowMinimumSizeResponse { Gfx::IntSize {} };
}

Messages::WindowServer::GetWindowTitleResponse WindowServerConnection::get_window_title(i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowTitleResponse { w->title };
    return Messages::WindowServer::GetWindowTitleResponse { ByteString {} };
}

Messages::WindowServer::IsWindowModifiedResponse WindowServerConnection::is_window_modified(i32)
{
    return Messages::WindowServer::IsWindowModifiedResponse { false };
}

Messages::WindowServer::GetAppletRectOnScreenResponse WindowServerConnection::get_applet_rect_on_screen(i32)
{
    return Messages::WindowServer::GetAppletRectOnScreenResponse { Gfx::IntRect {} };
}

Messages::WindowServer::GetWindowRectFromClientResponse WindowServerConnection::get_window_rect_from_client(i32, i32 window_id)
{
    if (auto* w = window(window_id))
        return Messages::WindowServer::GetWindowRectFromClientResponse { w->rect };
    return Messages::WindowServer::GetWindowRectFromClientResponse { Gfx::IntRect {} };
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

void WindowServerConnection::did_finish_painting(i32 window_id, Vector<Gfx::IntRect> const& rects)
{
    // The client reports exactly what it painted, before presenting. Record it as
    // the surface damage; the present that follows consumes it. This includes the
    // update() rects the client folded into the paint, which the invalidation
    // request may not have covered.
    WaylandClient::the().add_window_damage(window_id, rects);
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
    dbgln("LibWM: set_maximized({}, {})", window_id, maximized);
    WaylandClient::the().set_maximized(window_id, maximized);
}

void WindowServerConnection::set_minimized(i32 window_id, bool minimized)
{
    dbgln("LibWM: set_minimized({}, {})", window_id, minimized);
    if (minimized)
        WaylandClient::the().set_minimized(window_id);
}

void WindowServerConnection::set_always_on_top(i32 window_id, bool always_on_top)
{
    WaylandClient::the().set_always_on_top(window_id, always_on_top);
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
    return Messages::WindowServer::GetColorUnderCursorResponse { Optional<Gfx::Color> {} };
}

namespace {
// R5 accessibility (docs/29): the Magnifier's capture. Ask for the region and answer
// with the most recent one the compositor pushed; the first ask returns empty and
// the next display-link tick paints it (one refresh interval of latency).
Gfx::ShareableBitmap capture_around(Gfx::IntSize size, Gfx::IntPoint location)
{
    auto& wayland = WaylandClient::the();
    Gfx::IntRect const region { { location.x() - size.width() / 2, location.y() - size.height() / 2 }, size };
    wayland.request_capture(region);
    if (auto bitmap = wayland.captured_bitmap())
        return bitmap->to_shareable_bitmap();
    return Gfx::ShareableBitmap {};
}
}

Messages::WindowServer::GetScreenBitmapAroundCursorResponse WindowServerConnection::get_screen_bitmap_around_cursor(Gfx::IntSize size)
{
    return capture_around(size, WaylandClient::the().global_pointer_position());
}

Messages::WindowServer::GetScreenBitmapAroundLocationResponse WindowServerConnection::get_screen_bitmap_around_location(Gfx::IntSize size, Gfx::IntPoint location)
{
    return capture_around(size, location);
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
    // LibGUI computes the button position in screen coordinates, but a Wayland
    // popup is positioned relative to its parent surface. Convert by removing
    // the parent window's screen origin, which the compositor reported to us.
    auto position = screen_position;
    if (auto* w = window(parent))
        position.translate_by(-w->rect.location());

    auto anchor = button_rect.is_empty()
        ? Gfx::IntRect { position, { 1, 1 } }
        : Gfx::IntRect { position, button_rect.size() };
    m_menu.open_root(menu_id, parent, anchor);
}

void WindowServerConnection::dismiss_menu(i32 menu_id)
{
    m_menu.close_menu(menu_id);
}

}
