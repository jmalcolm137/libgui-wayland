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
#include <LibGUI/Dialog.h>
#include <LibGUI/FilePicker.h>
#include <LibIPC/Connection.h>
#include <LibIPC/File.h>
#include <FileSystemAccessServer/FileSystemAccessClientEndpoint.h>
#include <FileSystemAccessServer/FileSystemAccessServerEndpoint.h>

#include "FileSystemAccessServerDefaultStub.h"
#include "MainThreadInvoker.h"

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

    void prompt_open_file(i32 request_id, i32, i32, ByteString const& window_title, ByteString const& path_to_view, Core::File::OpenMode requested_access, Optional<Vector<GUI::FileTypeFilter>> const& allowed_file_types) override
    {
        MainThreadInvoker::post_to_main([this, request_id, window_title, path_to_view, requested_access, allowed_file_types] {
            auto chosen = GUI::FilePicker::get_open_filepath(nullptr, window_title, path_to_view, false, GUI::Dialog::ScreenPosition::Center, allowed_file_types);
            MainThreadInvoker::post_to_server([this, request_id, chosen, requested_access] {
                if (!chosen.has_value()) {
                    async_handle_prompt_end(request_id, ECANCELED, Optional<IPC::File> {}, Optional<ByteString> {});
                    return;
                }
                open_and_reply(request_id, *chosen, requested_access);
            });
        });
    }

    void prompt_save_file(i32 request_id, i32, i32, ByteString const& title, ByteString const& ext, ByteString const& path_to_view, Core::File::OpenMode requested_access) override
    {
        MainThreadInvoker::post_to_main([this, request_id, title, ext, path_to_view, requested_access] {
            auto chosen = GUI::FilePicker::get_save_filepath(nullptr, title, ext, path_to_view, GUI::Dialog::ScreenPosition::Center);
            MainThreadInvoker::post_to_server([this, request_id, chosen, requested_access] {
                if (!chosen.has_value()) {
                    async_handle_prompt_end(request_id, ECANCELED, Optional<IPC::File> {}, Optional<ByteString> {});
                    return;
                }
                open_and_reply(request_id, *chosen, requested_access);
            });
        });
    }

    Messages::FileSystemAccessServer::ExposeWindowServerClientIdResponse expose_window_server_client_id() override
    {
        return Messages::FileSystemAccessServer::ExposeWindowServerClientIdResponse(1);
    }
};

}
