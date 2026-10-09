/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WaylandClient.h"
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <Clipboard/ClipboardClientEndpoint.h>
#include <Clipboard/ClipboardServerEndpoint.h>
#include <string.h>

#include "ClipboardServerDefaultStub.h"

namespace LibWM {

// Bridges the SerenityOS clipboard protocol to the native Wayland data device.
//
// This runs on the LibWM server thread, the same thread that owns the Wayland
// connection, because wl_data_device.set_selection requires a serial from a
// recent input event on that connection's seat.
class ClipboardServerConnection final
    : public IPC::Connection<ClipboardServerEndpoint, ClipboardClientEndpoint>
    , public ClipboardServerDefaultStub
    , public ClipboardClientEndpoint::Proxy<ClipboardServerEndpoint> {
public:
    static NonnullRefPtr<ClipboardServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new ClipboardServerConnection(move(socket)));
    }

private:
    explicit ClipboardServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<ClipboardServerEndpoint, ClipboardClientEndpoint>(*this, move(socket))
        , ClipboardClientEndpoint::Proxy<ClipboardServerEndpoint>(*this, {})
    {
        WaylandClient::the().set_clipboard_changed_callback([this](ByteString const& mime_type) {
            async_clipboard_data_changed(mime_type);
        });
    }

    virtual Messages::ClipboardServer::GetClipboardDataResponse get_clipboard_data() override
    {
        ByteString mime_type;
        auto data = WaylandClient::the().read_clipboard(mime_type);
        if (data.is_error() || data.value().is_empty())
            return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };

        auto buffer = Core::AnonymousBuffer::create_with_size(data.value().size());
        if (buffer.is_error())
            return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };
        memcpy(buffer.value().data<void>(), data.value().data(), data.value().size());
        return { buffer.release_value(), move(mime_type), HashMap<ByteString, ByteString> {} };
    }

    virtual void set_clipboard_data(Core::AnonymousBuffer const& data, ByteString const& mime_type, HashMap<ByteString, ByteString> const&) override
    {
        if (!data.is_valid())
            return;
        WaylandClient::the().write_clipboard({ data.data<u8>(), data.size() }, mime_type);
    }
};

}
