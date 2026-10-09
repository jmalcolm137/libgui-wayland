/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <LibIPC/File.h>
#include <FileSystemAccessServer/FileSystemAccessClientEndpoint.h>
#include <FileSystemAccessServer/FileSystemAccessServerEndpoint.h>

#include "FileSystemAccessServerDefaultStub.h"

namespace LibWM {

// A minimal in-process FileSystemAccessServer.
//
// On SerenityOS this service shows a file picker and gates access through a
// permission prompt. On the host there is no picker and the application already
// names the file (e.g. a PDF passed on the command line), so "prompt" requests
// resolve to the requested path directly and approved requests just open it.
class FileSystemAccessServerConnection final
    : public IPC::Connection<FileSystemAccessServerEndpoint, FileSystemAccessClientEndpoint>
    , public FileSystemAccessServerDefaultStub
    , public FileSystemAccessClientEndpoint::Proxy<FileSystemAccessServerEndpoint> {
public:
    static NonnullRefPtr<FileSystemAccessServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new FileSystemAccessServerConnection(move(socket)));
    }

private:
    explicit FileSystemAccessServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<FileSystemAccessServerEndpoint, FileSystemAccessClientEndpoint>(*this, move(socket))
        , FileSystemAccessClientEndpoint::Proxy<FileSystemAccessServerEndpoint>(*this, {})
    {
    }

    void open_and_reply(i32 request_id, ByteString const& path, Core::File::OpenMode requested_access)
    {
        auto file = Core::File::open(path, requested_access);
        if (file.is_error()) {
            dbgln("LibWM/FileSystemAccess: couldn't open '{}': {}", path, file.error());
            async_handle_prompt_end(request_id, file.error().code(), Optional<IPC::File> {}, path);
            return;
        }
        async_handle_prompt_end(request_id, 0, IPC::File::adopt_file(file.release_value()), path);
    }

    void request_file_read_only_approved(i32 request_id, ByteString const& path) override
    {
        open_and_reply(request_id, path, Core::File::OpenMode::Read);
    }

    void request_file(i32 request_id, i32, i32, ByteString const& path, Core::File::OpenMode requested_access) override
    {
        open_and_reply(request_id, path, requested_access);
    }

    void prompt_open_file(i32 request_id, i32, i32, ByteString const&, ByteString const& path_to_view, Core::File::OpenMode requested_access, Optional<Vector<GUI::FileTypeFilter>> const&) override
    {
        if (path_to_view.is_empty())
            async_handle_prompt_end(request_id, ECANCELED, Optional<IPC::File> {}, Optional<ByteString> {});
        else
            open_and_reply(request_id, path_to_view, requested_access);
    }

    void prompt_save_file(i32 request_id, i32, i32, ByteString const&, ByteString const&, ByteString const& path_to_view, Core::File::OpenMode requested_access) override
    {
        open_and_reply(request_id, path_to_view, requested_access);
    }

    Messages::FileSystemAccessServer::ExposeWindowServerClientIdResponse expose_window_server_client_id() override
    {
        return Messages::FileSystemAccessServer::ExposeWindowServerClientIdResponse(1);
    }
};

}
