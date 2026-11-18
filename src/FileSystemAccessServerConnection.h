/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibGUI/FileTypeFilter.h>
#include <LibIPC/Connection.h>
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
    explicit FileSystemAccessServerConnection(NonnullOwnPtr<Core::LocalSocket> socket);

    void open_and_reply(i32 request_id, ByteString const& path, Core::File::OpenMode requested_access);

    void request_file_read_only_approved(i32 request_id, ByteString const& path) override;
    void request_file(i32 request_id, i32, i32, ByteString const& path, Core::File::OpenMode requested_access) override;
    void prompt_open_file(i32 request_id, i32, i32, ByteString const& window_title, ByteString const& path_to_view, Core::File::OpenMode requested_access, Optional<Vector<GUI::FileTypeFilter>> const& allowed_file_types) override;
    void prompt_save_file(i32 request_id, i32, i32, ByteString const& title, ByteString const& ext, ByteString const& path_to_view, Core::File::OpenMode requested_access) override;
    Messages::FileSystemAccessServer::ExposeWindowServerClientIdResponse expose_window_server_client_id() override;
};

}
