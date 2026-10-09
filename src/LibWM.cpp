/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include "ClipboardServerConnection.h"
#include "LaunchServerConnection.h"
#include "WindowServerConnection.h"
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/EventLoop.h>
#include <LibCore/PortalServer.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/Socket.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/SystemTheme.h>
#include <LibThreading/Thread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

namespace LibWM {

static RefPtr<WindowServerConnection> s_window_server;
static RefPtr<LaunchServerConnection> s_launch_server;
static RefPtr<ClipboardServerConnection> s_clipboard_server;

// Absolute path to the bundled SerenityOS resource tree (fonts, themes, icons).
static ByteString resource_root()
{
    if (auto const* env = getenv("LIBWM_RES"); env && *env)
        return ByteString(env);
    if (auto const* env = getenv("SERENITY_RES_ROOT"); env && *env)
        return ByteString(env);
    return ByteString(LIBWM_DEFAULT_RES);
}

static void ensure_theme_installed()
{
    static bool s_initialized = false;
    if (s_initialized)
        return;
    s_initialized = true;

    auto root = resource_root();
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(root))));

    auto theme_path = ByteString::formatted("{}/themes/Default.ini", root);
    auto scheme_path = ByteString::formatted("{}/color-schemes/Default.ini", root);
    if (auto buffer = Gfx::load_system_theme(theme_path, scheme_path); !buffer.is_error()) {
        Gfx::set_system_theme(buffer.release_value());
    } else {
        dbgln("LibWM: failed to load system theme: {}", buffer.error());
    }
}

static Optional<NonnullOwnPtr<Core::LocalSocket>> make_portal(ByteString const& path)
{
    dbgln("LibWM: portal connect request '{}'", path);

    bool const is_window = path == "/tmp/portal/window"sv;
    bool const is_launch = path.ends_with("/portal/launch"sv);
    bool const is_clipboard = path.ends_with("/portal/clipboard"sv);
    if (!is_window && !is_launch && !is_clipboard)
        return {};

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return {};

    if (is_window) {
        ensure_theme_installed();

        // The client blocks on FastGreet in ConnectionToWindowServer's
        // constructor, and makes synchronous calls (e.g. get_window_rect)
        // before entering its event loop, so the server must run on its own
        // thread with its own event loop.
        auto server_fd = fds[0];
        auto thread = Threading::Thread::try_create([server_fd]() -> intptr_t {
            auto socket = Core::LocalSocket::adopt_fd(server_fd);
            if (socket.is_error())
                return 1;
            auto connection = WindowServerConnection::create(socket.release_value());
            s_window_server = connection;
            connection->send_fast_greet();
            Core::EventLoop loop;
            loop.exec();
            return 0;
        }, "LibWM WindowServer"sv);

        if (thread.is_error()) {
            ::close(fds[0]);
            ::close(fds[1]);
            return {};
        }
        thread.value()->start();
        thread.value()->detach();

        auto client_socket = Core::LocalSocket::adopt_fd(fds[1]);
        if (client_socket.is_error())
            return {};
        return client_socket.release_value();
    }

    if (is_launch) {
        // Launcher allowlist registration is synchronous during app startup, so
        // (like the WindowServer) it needs its own event-loop thread.
        auto server_fd = fds[0];
        auto thread = Threading::Thread::try_create([server_fd]() -> intptr_t {
            dbgln("LibWM: LaunchServer thread starting");
            auto socket = Core::LocalSocket::adopt_fd(server_fd);
            if (socket.is_error())
                return 1;
            auto connection = LaunchServerConnection::create(socket.release_value());
            s_launch_server = connection;
            dbgln("LibWM: LaunchServer connection ready");
            Core::EventLoop loop;
            loop.exec();
            return 0;
        }, "LibWM LaunchServer"sv);

        if (thread.is_error()) {
            ::close(fds[0]);
            ::close(fds[1]);
            return {};
        }
        thread.value()->start();
        thread.value()->detach();

        auto client_socket = Core::LocalSocket::adopt_fd(fds[1]);
        if (client_socket.is_error())
            return {};
        return client_socket.release_value();
    }

    // Clipboard calls are synchronous (e.g. TextEditor reads it at startup), so
    // the server runs on its own event-loop thread too.
    auto server_fd = fds[0];
    auto thread = Threading::Thread::try_create([server_fd]() -> intptr_t {
        auto socket = Core::LocalSocket::adopt_fd(server_fd);
        if (socket.is_error())
            return 1;
        auto connection = ClipboardServerConnection::create(socket.release_value());
        s_clipboard_server = connection;
        Core::EventLoop loop;
        loop.exec();
        return 0;
    }, "LibWM ClipboardServer"sv);

    if (thread.is_error()) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {};
    }
    thread.value()->start();
    thread.value()->detach();

    auto client_socket = Core::LocalSocket::adopt_fd(fds[1]);
    if (client_socket.is_error())
        return {};
    return client_socket.release_value();
}

void initialize()
{
    ensure_theme_installed();
    Core::PortalServer::set_connector([](ByteString const& path) {
        return make_portal(path);
    });
}

namespace {
struct AutoInitializer {
    AutoInitializer() { initialize(); }
};
AutoInitializer s_auto_initializer;
}

}
