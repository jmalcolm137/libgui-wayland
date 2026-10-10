/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Connection.h>
#include <LibURL/URL.h>
#include <LaunchServer/LaunchClientEndpoint.h>
#include <LaunchServer/LaunchServerEndpoint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "LaunchServerDefaultStub.h"

namespace LibWM {

// A minimal in-process LaunchServer. It resolves a file or URL to a handler
// using the LaunchServer.ini [FileType]/[Protocol] maps and spawns the matching
// application from this build's bin directory (so "/bin/TextEditor" launches the
// host binary next to the running executable).
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
            executable = default_executable_for_url(url);
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
        Vector<ByteString> handlers;
        if (auto executable = default_executable_for_url(url); !executable.is_empty())
            handlers.append(executable);
        return handlers;
    }

    virtual Messages::LaunchServer::GetHandlersWithDetailsForUrlResponse get_handlers_with_details_for_url(URL::URL const& url) override
    {
        load_handlers_if_needed();
        Vector<ByteString> handlers;
        if (auto executable = default_executable_for_url(url); !executable.is_empty())
            handlers.append(details_for(executable));
        return handlers;
    }

    void load_handlers_if_needed()
    {
        if (m_loaded)
            return;
        m_loaded = true;

        ByteString config_path { "/etc/LaunchServer.ini" };
        if (auto const* res_root = getenv("SERENITY_RES_ROOT"); res_root && *res_root) {
            auto candidate = ByteString::formatted("{}/../etc/LaunchServer.ini", res_root);
            if (!Core::System::access(candidate, R_OK).is_error())
                config_path = move(candidate);
        }

        auto config = Core::ConfigFile::open(config_path).release_value_but_fixme_should_propagate_errors();
        for (auto& key : config->keys("FileType"))
            m_file_handlers.set(key, config->read_entry("FileType", key).trim_whitespace());
        for (auto& key : config->keys("Protocol"))
            m_protocol_handlers.set(key, config->read_entry("Protocol", key).trim_whitespace());
    }

    ByteString default_executable_for_url(URL::URL const& url)
    {
        if (url.scheme() == "file"sv) {
            auto path = URL::percent_decode(url.serialize_path());
            struct stat st;
            if (!path.is_empty() && ::stat(path.characters(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    if (auto handler = m_file_handlers.get("directory"sv); handler.has_value())
                        return *handler;
                }
            }
            auto extension = LexicalPath(path).extension();
            if (!extension.is_empty()) {
                if (auto handler = m_file_handlers.get(extension.to_byte_string()); handler.has_value())
                    return *handler;
            }
            if (auto handler = m_file_handlers.get("*"sv); handler.has_value())
                return *handler;
            return {};
        }
        if (auto handler = m_protocol_handlers.get(url.scheme().to_byte_string()); handler.has_value())
            return *handler;
        return {};
    }

    // "/bin/TextEditor" -> "<dir of this executable>/TextEditor", so the handler
    // runs the host binary rather than the (nonexistent) Serenity /bin path.
    static ByteString port_binary_path(StringView executable)
    {
        auto name = LexicalPath(executable).basename();
        char buffer[4096];
        auto length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0)
            return executable.to_byte_string();
        buffer[length] = '\0';
        return ByteString::formatted("{}/{}", LexicalPath { StringView { buffer, static_cast<size_t>(length) } }.dirname(), name);
    }

    static ByteString details_for(ByteString const& executable)
    {
        auto name = LexicalPath(executable).basename();
        return ByteString::formatted(R"({{"executable":"{}","name":"{}","arguments":[],"type":"app"}})", executable, name);
    }

    HashMap<ByteString, ByteString> m_file_handlers;
    HashMap<ByteString, ByteString> m_protocol_handlers;
    bool m_loaded { false };
};

}
