/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LaunchServerConnection.h"
#include "Environment.h"
#include <AK/LexicalPath.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <sys/stat.h>
#include <unistd.h>

namespace LibWM {

LaunchServerConnection::LaunchServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::Connection<LaunchServerEndpoint, LaunchClientEndpoint>(*this, move(socket))
    , LaunchClientEndpoint::Proxy<LaunchServerEndpoint>(*this, {})
{
}

void LaunchServerConnection::add_allowed_handler_with_only_specific_urls(ByteString const& handler_name, Vector<URL::URL> const& urls)
{
    dbgln("LibWM: LaunchServer allowlist handler '{}' ({} urls)", handler_name, urls.size());
}

void LaunchServerConnection::seal_allowlist()
{
    dbgln("LibWM: LaunchServer seal_allowlist");
}

Messages::LaunchServer::OpenUrlResponse LaunchServerConnection::open_url(URL::URL const& url, ByteString const& handler_name)
{
    load_handlers_if_needed();

    // A file URL that points at an executable runs it directly, mirroring the
    // upstream LaunchServer's open_file_url(). This is how the desktop's
    // "Display Settings" entry (Desktop::Launcher::open("/bin/DisplaySettings"))
    // starts the same app the Settings app spawns. "/bin/X" is mapped to the
    // session app directory via port_binary_path(), because on the host the
    // Serenity "/bin" does not exist.
    if (handler_name.is_empty() && url.scheme() == "file"sv) {
        auto file_path = URL::percent_decode(url.serialize_path());
        if (file_path.starts_with("/bin/"sv))
            file_path = port_binary_path(file_path);
        struct stat st;
        if (!file_path.is_empty() && ::stat(file_path.characters(), &st) == 0
            && S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            if (auto result = Core::Process::spawn(file_path, ReadonlySpan<ByteString> {}); result.is_error()) {
                dbgln("LibWM: LaunchServer: failed to launch executable '{}': {}", file_path, result.error());
                return false;
            }
            dbgln("LibWM: LaunchServer: launched executable '{}'", file_path);
            return true;
        }
    }

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

Messages::LaunchServer::GetHandlersForUrlResponse LaunchServerConnection::get_handlers_for_url(URL::URL const& url)
{
    load_handlers_if_needed();
    return m_handlers.handlers_for_url(url);
}

Messages::LaunchServer::GetHandlersWithDetailsForUrlResponse LaunchServerConnection::get_handlers_with_details_for_url(URL::URL const& url)
{
    load_handlers_if_needed();
    Vector<ByteString> details;
    for (auto const& executable : m_handlers.handlers_for_url(url))
        details.append(details_for(executable));
    return details;
}

void LaunchServerConnection::load_handlers_if_needed()
{
    if (m_loaded)
        return;
    m_loaded = true;

    if (auto database = Launch::HandlerDatabase::load(); database.is_error())
        dbgln("LibWM: LaunchServer: could not load handlers: {}", database.error());
    else
        m_handlers = database.release_value();
}

ByteString LaunchServerConnection::port_binary_path(StringView executable)
{
    // LexicalPath::basename() returns a StringView *into* the LexicalPath, so the
    // owner must outlive it: `auto name = LexicalPath(executable).basename();`
    // leaves `name` pointing at a destroyed temporary and the path built from it
    // is garbage. That is not theoretical — it made an executable file URL
    // resolve to a nonsense path, fail stat(), and fall through to the TextEditor
    // handler, so the desktop's "Display Settings" entry never launched.
    LexicalPath const lexical_path { executable.to_byte_string() };
    auto name = lexical_path.basename();
    if (auto app_bin_dir = Environment::app_bin_dir(); !app_bin_dir.is_empty())
        return ByteString::formatted("{}/{}", app_bin_dir, name);
    char buffer[4096];
    auto length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0)
        return executable.to_byte_string();
    buffer[length] = '\0';
    LexicalPath const self_path { StringView { buffer, static_cast<size_t>(length) } };
    return ByteString::formatted("{}/{}", self_path.dirname(), name);
}

ByteString LaunchServerConnection::details_for(ByteString const& executable)
{
    LexicalPath const lexical_path { executable };
    auto name = lexical_path.basename();
    // No "type" field: the client treats the default (non-Application) type
    // as "open this file with the program", passing the path as an argument.
    return ByteString::formatted(R"({{"executable":"{}","name":"{}","arguments":[]}})", executable, name);
}

}
