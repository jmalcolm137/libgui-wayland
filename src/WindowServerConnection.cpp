/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowServerConnection.h"
#include "WaylandClient.h"
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <fcntl.h>
#include <stdlib.h>
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

    auto& wayland = WaylandClient::the();
    if (auto result = wayland.ensure_connected(); result.is_error())
        dbgln("LibWM: running without a Wayland compositor: {}", result.error());

    auto screen = wayland.screen_size();
    if (screen.is_empty())
        screen = m_screen_size;

    Vector<Gfx::IntRect> screen_rects;
    screen_rects.append({ 0, 0, screen.width(), screen.height() });
    Vector<bool> effects;
    async_fast_greet(
        move(screen_rects),
        0, 1, 1,
        Gfx::current_system_theme_buffer(),
        "Katica 10 400 0"sv,
        "Csilla 10 400 0"sv,
        "Katica 10 700 0"sv,
        move(effects),
        m_client_id);
}

void WindowServerConnection::send_paint(Window& window, Vector<Gfx::IntRect> rects)
{
    async_paint(window.id, window.rect.size(), move(rects));
}

void WindowServerConnection::install_input_callbacks()
{
    WaylandClient::InputCallbacks callbacks;

    callbacks.mouse_move = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers) {
        async_mouse_move(window_id, position, 0, buttons, modifiers, 0, 0, 0, 0);
    };
    callbacks.mouse_down = [this](i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers) {
        async_mouse_down(window_id, position, button, buttons, modifiers, 0, 0, 0, 0);
    };
    callbacks.mouse_up = [this](i32 window_id, Gfx::IntPoint position, u32 button, u32 buttons, u32 modifiers) {
        async_mouse_up(window_id, position, button, buttons, modifiers, 0, 0, 0, 0);
    };
    callbacks.mouse_wheel = [this](i32 window_id, Gfx::IntPoint position, u32 buttons, u32 modifiers, i32 wheel_delta_x, i32 wheel_delta_y) {
        async_mouse_wheel(window_id, position, 0, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_x, wheel_delta_y);
    };
    callbacks.key = [this](i32 window_id, u32 code_point, u32 key, u8 map_entry_index, u32 modifiers, u32 scancode, bool is_press) {
        if (is_press)
            async_key_down(window_id, code_point, key, map_entry_index, modifiers, scancode);
        else
            async_key_up(window_id, code_point, key, map_entry_index, modifiers, scancode);
    };
    callbacks.window_entered = [this](i32 window_id) { async_window_entered(window_id); };
    callbacks.window_left = [this](i32 window_id) { async_window_left(window_id); };
    callbacks.window_close_request = [this](i32 window_id) { async_window_close_request(window_id); };
    callbacks.window_activation = [this](i32 window_id, bool activated) {
        if (activated)
            async_window_activated(window_id);
        else
            async_window_deactivated(window_id);
    };

    WaylandClient::the().set_input_callbacks(move(callbacks));
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

void WindowServerConnection::create_window(i32 window_id, i32, Gfx::IntRect const& rect, bool, bool has_alpha_channel, bool, bool, bool, bool, bool, bool, float, Gfx::IntSize, Gfx::IntSize, Gfx::IntSize minimum_size, Optional<Gfx::IntSize> const&, i32, i32, ByteString const& title, i32, Gfx::IntRect const&)
{
    auto window = make<Window>();
    window->id = window_id;
    window->rect = rect;
    window->minimum_size = minimum_size;
    window->has_alpha_channel = has_alpha_channel;
    window->title = title;

    dbgln("LibWM: create_window id={} rect={},{},{}x{} title='{}'", window_id, rect.x(), rect.y(), rect.width(), rect.height(), title);
    m_windows.append(move(window));

    WaylandClient::the().create_window(window_id, rect.size(), title, has_alpha_channel);

    // Ask the client to paint the whole window.
    Vector<Gfx::IntRect> rects;
    rects.append({ 0, 0, rect.width(), rect.height() });
    send_paint(*m_windows.last(), move(rects));
}

void WindowServerConnection::set_window_title(i32 window_id, ByteString const& title)
{
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

void WindowServerConnection::set_window_backing_store(i32 window_id, i32, i32 pitch, IPC::File const& anon_file, i32 serial, bool has_alpha_channel, Gfx::IntSize size, Gfx::IntSize, bool)
{
    auto* w = window(window_id);
    if (!w) {
        dbgln("LibWM: backing store for unknown window {}", window_id);
        return;
    }

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
    WaylandClient::the().attach_and_commit(window_id, anon_file.fd(), size, pitch, has_alpha_channel);
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

void WindowServerConnection::set_window_icon_bitmap(i32, Gfx::ShareableBitmap const&)
{
}

void WindowServerConnection::set_window_progress(i32, Optional<i32> const&)
{
}

void WindowServerConnection::move_window_to_front(i32)
{
}

Messages::WindowServer::GetGlobalCursorPositionResponse WindowServerConnection::get_global_cursor_position()
{
    return Messages::WindowServer::GetGlobalCursorPositionResponse { Gfx::IntPoint {} };
}

Messages::WindowServer::GetColorUnderCursorResponse WindowServerConnection::get_color_under_cursor()
{
    return Messages::WindowServer::GetColorUnderCursorResponse { Optional<Gfx::Color> {} };
}

Messages::WindowServer::GetWallpaperResponse WindowServerConnection::get_wallpaper()
{
    return Messages::WindowServer::GetWallpaperResponse { Gfx::ShareableBitmap {} };
}

Messages::WindowServer::SetWallpaperResponse WindowServerConnection::set_wallpaper(Gfx::ShareableBitmap const&)
{
    return Messages::WindowServer::SetWallpaperResponse { true };
}

Messages::WindowServer::StartDragResponse WindowServerConnection::start_drag(ByteString const&, HashMap<String, ByteBuffer> const&, Gfx::ShareableBitmap const&)
{
    return Messages::WindowServer::StartDragResponse { false };
}

}
