/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Connection.h>
#include <LibLaunch/HandlerDatabase.h>
#include <LibURL/URL.h>
#include <LaunchServer/LaunchClientEndpoint.h>
#include <LaunchServer/LaunchServerEndpoint.h>
#include <stdlib.h>
#include <unistd.h>

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

    virtual Messages::LaunchServer::OpenUrlResponse open_url(URL::URL const& url, ByteString const& handler_name) override
    {
        load_handlers_if_needed();

        auto executable = handler_name;
        if (executable.is_empty())
            executable = m_handlers.default_executable_for_url(url);
        if (executable.is_empty()) {
            dbgln("LibWM: LaunchServer: no handler for '{}'", url.to_byte_string());
            return false;
        }

        ByteString argument;
        if (url.scheme() == "file"sv)
            argument = URL::percent_decode(url.serialize_path());
        else
            argument = url.to_byte_string();

        auto path = port_binary_path(executable);
        if (Core::System::access(path, X_OK).is_error() && url.scheme() == "file"sv) {
            // The configured handler isn't built here; fall back to the editor.
            path = port_binary_path("/bin/TextEditor"sv);
        }

        Vector<ByteString> arguments;
        if (!argument.is_empty())
            arguments.append(argument);

        auto result = Core::Process::spawn(path, arguments);
        if (result.is_error()) {
            dbgln("LibWM: LaunchServer: failed to spawn '{}': {}", path, result.error());
            return false;
        }
        dbgln("LibWM: LaunchServer: launched '{}' with '{}'", path, argument);
        return true;
    }

    virtual Messages::LaunchServer::GetHandlersForUrlResponse get_handlers_for_url(URL::URL const& url) override
    {
        load_handlers_if_needed();
        return m_handlers.handlers_for_url(url);
    }

    virtual Messages::LaunchServer::GetHandlersWithDetailsForUrlResponse get_handlers_with_details_for_url(URL::URL const& url) override
    {
        load_handlers_if_needed();
        Vector<ByteString> details;
        for (auto const& executable : m_handlers.handlers_for_url(url))
            details.append(details_for(executable));
        return details;
    }

    void load_handlers_if_needed()
    {
        if (m_loaded)
            return;
        m_loaded = true;

        if (auto database = Launch::HandlerDatabase::load(); database.is_error())
            dbgln("LibWM: LaunchServer: could not load handlers: {}", database.error());
        else
            m_handlers = database.release_value();
    }

    // "/bin/TextEditor" -> "<dir of this executable>/TextEditor", so the handler
    // runs the host binary rather than the (nonexistent) Serenity /bin path.
    // A standalone session service (the SDE LaunchServer) is not built next to
    // the applications, so it points at their directory with SDE_APP_BIN_DIR.
    static ByteString port_binary_path(StringView executable)
    {
        auto name = LexicalPath(executable).basename();
        if (auto const* app_bin_dir = getenv("SDE_APP_BIN_DIR"); app_bin_dir && *app_bin_dir)
            return ByteString::formatted("{}/{}", app_bin_dir, name);
        char buffer[4096];
        auto length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0)
            return executable.to_byte_string();
        buffer[length] = '\0';
        LexicalPath const self_path { StringView { buffer, static_cast<size_t>(length) } };
        return ByteString::formatted("{}/{}", self_path.dirname(), name);
    }

    static ByteString details_for(ByteString const& executable)
    {
        auto name = LexicalPath(executable).basename();
        // No "type" field: the client treats the default (non-Application) type
        // as "open this file with the program", passing the path as an argument.
        return ByteString::formatted(R"({{"executable":"{}","name":"{}","arguments":[]}})", executable, name);
    }

    Launch::HandlerDatabase m_handlers;
    bool m_loaded { false };
};

}
