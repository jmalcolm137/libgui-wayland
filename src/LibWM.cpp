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
#include <LibCore/SessionManagement.h>
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

// Client ends of the portal socketpairs, handed out on connect.
static HashMap<ByteString, int> s_client_fds;
static bool s_server_started { false };

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

static ByteString expanded_portal_path(StringView template_path)
{
    return MUST(Core::SessionManagement::parse_path_with_sid(template_path));
}

// All portals are served from a single thread with a single Core::EventLoop.
//
// This matters for two reasons: the client makes synchronous portal calls
// outside its own event loop (so the servers cannot live on the client thread),
// and the clipboard must use the same Wayland connection as input, because
// wl_data_device.set_selection requires a serial from a recent input event.
static void start_server_thread()
{
    auto make_socketpair = [](int& server_fd) -> int {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            return -1;
        server_fd = fds[0];
        return fds[1];
    };

    int window_server = -1;
    int clipboard_server = -1;
    int launch_server = -1;
    int window_client = make_socketpair(window_server);
    int clipboard_client = make_socketpair(clipboard_server);
    int launch_client = make_socketpair(launch_server);
    if (window_client < 0 || clipboard_client < 0 || launch_client < 0) {
        dbgln("LibWM: failed to create portal socketpairs");
        return;
    }

    s_client_fds.set("/tmp/portal/window"sv, window_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/clipboard"sv), clipboard_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/launch"sv), launch_client);

    auto thread = Threading::Thread::try_create([window_server, clipboard_server, launch_server]() -> intptr_t {
        Core::EventLoop loop;

        // WindowServer first: this connects to Wayland and installs the input
        // callbacks the clipboard also relies on.
        if (auto socket = Core::LocalSocket::adopt_fd(window_server); !socket.is_error()) {
            auto connection = WindowServerConnection::create(socket.release_value());
            s_window_server = connection;
            connection->send_fast_greet();
        }
        if (auto socket = Core::LocalSocket::adopt_fd(clipboard_server); !socket.is_error())
            s_clipboard_server = ClipboardServerConnection::create(socket.release_value());
        if (auto socket = Core::LocalSocket::adopt_fd(launch_server); !socket.is_error())
            s_launch_server = LaunchServerConnection::create(socket.release_value());

        loop.exec();
        return 0;
    }, "LibWM Server"sv);

    if (thread.is_error()) {
        dbgln("LibWM: failed to start server thread: {}", thread.error());
        ::close(window_server);
        ::close(clipboard_server);
        ::close(launch_server);
        s_client_fds.clear();
        return;
    }
    thread.value()->start();
    thread.value()->detach();
}

static Optional<NonnullOwnPtr<Core::LocalSocket>> make_portal(ByteString const& path)
{
    dbgln("LibWM: portal connect request '{}'", path);

    if (!s_server_started) {
        s_server_started = true;
        start_server_thread();
    }

    auto client_fd = s_client_fds.take(path);
    if (!client_fd.has_value())
        return {};

    auto socket = Core::LocalSocket::adopt_fd(client_fd.value());
    if (socket.is_error())
        return {};
    return socket.release_value();
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
