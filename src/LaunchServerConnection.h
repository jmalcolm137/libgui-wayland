/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <LibLaunch/HandlerDatabase.h>
#include <LibURL/URL.h>
#include <LaunchServer/LaunchClientEndpoint.h>
#include <LaunchServer/LaunchServerEndpoint.h>

#include "LaunchServerDefaultStub.h"

namespace LibWM {

// The in-process LaunchServer fallback, used when no session LaunchServer is
// present (i.e. libgui-wayland running stand-alone under any compositor).
// Handler resolution is delegated to Launch::HandlerDatabase, the same shared
// resolver the SDE session LaunchServer uses, so an application's declared
// handlers behave identically either way. Spawning resolves "/bin/X" to the host
// binary next to the running executable.
class LaunchServerConnection final
    : public IPC::Connection<LaunchServerEndpoint, LaunchClientEndpoint>
    , public LaunchServerDefaultStub
    , public LaunchClientEndpoint::Proxy<LaunchServerEndpoint> {
public:
    static NonnullRefPtr<LaunchServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new LaunchServerConnection(move(socket)));
    }

private:
    explicit LaunchServerConnection(NonnullOwnPtr<Core::LocalSocket> socket);

    virtual void add_allowed_handler_with_only_specific_urls(ByteString const& handler_name, Vector<URL::URL> const& urls) override;
    virtual void seal_allowlist() override;
    virtual Messages::LaunchServer::OpenUrlResponse open_url(URL::URL const& url, ByteString const& handler_name) override;
    virtual Messages::LaunchServer::GetHandlersForUrlResponse get_handlers_for_url(URL::URL const& url) override;
    virtual Messages::LaunchServer::GetHandlersWithDetailsForUrlResponse get_handlers_with_details_for_url(URL::URL const& url) override;

    void load_handlers_if_needed();
    // "/bin/TextEditor" -> "<dir of this executable>/TextEditor", so the handler
    // runs the host binary rather than the (nonexistent) Serenity /bin path.
    // A standalone session service (the SDE LaunchServer) is not built next to
    // the applications, so it points at their directory with the session's app
    // directory: LIBWM_APP_BIN_DIR (generic) or SDE_APP_BIN_DIR (its older name).
    static ByteString port_binary_path(StringView executable);
    static ByteString details_for(ByteString const& executable);

    Launch::HandlerDatabase m_handlers;
    bool m_loaded { false };
};

}
