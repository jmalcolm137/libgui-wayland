/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include "ClipboardServerConnection.h"
#include "ConfigServerConnection.h"
#include "FileSystemAccessServerConnection.h"
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

// These keep the in-process server connections alive for the whole process.
// They are intentionally never destroyed: the servers run on their own thread,
// and tearing the IPC connections down during static destruction (__cxa_finalize)
// races that thread and the event loop, aborting at exit.
static RefPtr<WindowServerConnection>& window_server_slot()
{
    static auto* slot = new RefPtr<WindowServerConnection>;
    return *slot;
}
static RefPtr<LaunchServerConnection>& launch_server_slot()
{
    static auto* slot = new RefPtr<LaunchServerConnection>;
    return *slot;
}
static RefPtr<ClipboardServerConnection>& clipboard_server_slot()
{
    static auto* slot = new RefPtr<ClipboardServerConnection>;
    return *slot;
}
static RefPtr<ConfigServerConnection>& config_server_slot()
{
    static auto* slot = new RefPtr<ConfigServerConnection>;
    return *slot;
}
static RefPtr<FileSystemAccessServerConnection>& file_system_access_server_slot()
{
    static auto* slot = new RefPtr<FileSystemAccessServerConnection>;
    return *slot;
}

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

    // The server draws the menubar and menus, so it needs fonts of its own.
    Gfx::FontDatabase::set_default_font_query("Katica 10 400 0"sv);
    Gfx::FontDatabase::set_fixed_width_font_query("Csilla 10 400 0"sv);
    Gfx::FontDatabase::set_window_title_font_query("Katica 10 700 0"sv);
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
    int config_server = -1;
    int file_system_access_server = -1;
    int window_client = make_socketpair(window_server);
    int clipboard_client = make_socketpair(clipboard_server);
    int launch_client = make_socketpair(launch_server);
    int config_client = make_socketpair(config_server);
    int file_system_access_client = make_socketpair(file_system_access_server);
    if (window_client < 0 || clipboard_client < 0 || launch_client < 0 || config_client < 0 || file_system_access_client < 0) {
        dbgln("LibWM: failed to create portal socketpairs");
        return;
    }

    s_client_fds.set("/tmp/portal/window"sv, window_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/clipboard"sv), clipboard_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/launch"sv), launch_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/config"sv), config_client);
    s_client_fds.set(expanded_portal_path("/tmp/session/%sid/portal/filesystemaccess"sv), file_system_access_client);

    auto thread = Threading::Thread::try_create([window_server, clipboard_server, launch_server, config_server, file_system_access_server]() -> intptr_t {
        Core::EventLoop loop;

        // WindowServer first: this connects to Wayland and installs the input
        // callbacks the clipboard also relies on.
        if (auto socket = Core::LocalSocket::adopt_fd(window_server); !socket.is_error()) {
            auto connection = WindowServerConnection::create(socket.release_value());
            window_server_slot() = connection;
            connection->send_fast_greet();
        }
        if (auto socket = Core::LocalSocket::adopt_fd(clipboard_server); !socket.is_error())
            clipboard_server_slot() = ClipboardServerConnection::create(socket.release_value());
        if (auto socket = Core::LocalSocket::adopt_fd(launch_server); !socket.is_error())
            launch_server_slot() = LaunchServerConnection::create(socket.release_value());
        if (auto socket = Core::LocalSocket::adopt_fd(config_server); !socket.is_error())
            config_server_slot() = ConfigServerConnection::create(socket.release_value());
        if (auto socket = Core::LocalSocket::adopt_fd(file_system_access_server); !socket.is_error())
            file_system_access_server_slot() = FileSystemAccessServerConnection::create(socket.release_value());

        loop.exec();
        return 0;
    }, "LibWM Server"sv);

    if (thread.is_error()) {
        dbgln("LibWM: failed to start server thread: {}", thread.error());
        ::close(window_server);
        ::close(clipboard_server);
        ::close(launch_server);
        ::close(config_server);
        ::close(file_system_access_server);
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

    // Make SerenityOS's absolute "/res/..." paths resolve to our bundled tree
    // for code that opens them directly (LibCore::System::openat redirects).
    if (!getenv("SERENITY_RES_ROOT")) {
        auto root = resource_root();
        setenv("SERENITY_RES_ROOT", root.characters(), 0);
    }

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
