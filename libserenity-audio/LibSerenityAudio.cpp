/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibSerenityAudio.h"
#include "AudioServerConnection.h"
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/EventLoop.h>
#include <LibCore/PortalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/Socket.h>
#include <LibThreading/Thread.h>
#include <pipewire/pipewire.h>
#include <sys/socket.h>
#include <unistd.h>

namespace LibSerenityAudio {

// Kept alive for the whole process; the audio connection is torn down via its
// die() when the client disconnects, not by destroying this slot.
static RefPtr<AudioServerConnection>& connection_slot()
{
    static auto* slot = new RefPtr<AudioServerConnection>;
    return *slot;
}

static int s_client_fd { -1 };
static bool s_server_started { false };
static bool s_initialized { false };

static ByteString expanded_portal_path(StringView template_path)
{
    return MUST(Core::SessionManagement::parse_path_with_sid(template_path));
}

// A single thread with a single event loop serves the audio connection. The
// real-time mixing happens on PipeWire's own thread.
static void start_server_thread()
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        dbgln("LibSerenityAudio: failed to create the portal socketpair");
        return;
    }
    int server_fd = fds[0];
    s_client_fd = fds[1];

    auto thread = Threading::Thread::try_create([server_fd]() -> intptr_t {
        Core::EventLoop loop;
        if (auto socket = Core::LocalSocket::adopt_fd(server_fd); !socket.is_error()) {
            auto connection = AudioServerConnection::create(socket.release_value());
            connection_slot() = connection;
        }
        loop.exec();
        return 0;
    }, "LibSerenityAudio"sv);

    if (thread.is_error()) {
        dbgln("LibSerenityAudio: failed to start the server thread: {}", thread.error());
        ::close(server_fd);
        ::close(s_client_fd);
        s_client_fd = -1;
        return;
    }
    thread.value()->start();
    thread.value()->detach();
}

static Optional<NonnullOwnPtr<Core::LocalSocket>> make_portal(ByteString const& path)
{
    // Only the audio portal is ours; decline anything else so the next connector
    // (LibWM) or the filesystem can handle it.
    if (path != expanded_portal_path("/tmp/session/%sid/portal/audio"sv))
        return {};

    dbgln("LibSerenityAudio: portal connect request '{}'", path);
    if (!s_server_started) {
        s_server_started = true;
        start_server_thread();
    }
    if (s_client_fd < 0)
        return {};

    int fd = s_client_fd;
    s_client_fd = -1;
    auto socket = Core::LocalSocket::adopt_fd(fd);
    if (socket.is_error())
        return {};
    return socket.release_value();
}

void initialize()
{
    if (s_initialized)
        return;
    s_initialized = true;
    pw_init(nullptr, nullptr);
    Core::PortalServer::add_connector([](ByteString const& path) {
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
