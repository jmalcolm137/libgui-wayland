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
#include <LaunchServer/LaunchClientEndpoint.h>
#include <LaunchServer/LaunchServerEndpoint.h>

#include "LaunchServerDefaultStub.h"

namespace LibWM {

// A minimal in-process LaunchServer. Applications call the launcher during
// startup to register allowed handlers; the void allowlist calls succeed via the
// generated empty responses. Actual launching is not yet implemented.
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
    explicit LaunchServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<LaunchServerEndpoint, LaunchClientEndpoint>(*this, move(socket))
        , LaunchClientEndpoint::Proxy<LaunchServerEndpoint>(*this, {})
    {
    }

    virtual void add_allowed_handler_with_only_specific_urls(ByteString const& handler_name, Vector<URL::URL> const& urls) override
    {
        dbgln("LibWM: LaunchServer allowlist handler '{}' ({} urls)", handler_name, urls.size());
    }

    virtual void seal_allowlist() override
    {
        dbgln("LibWM: LaunchServer seal_allowlist");
    }
};

}
