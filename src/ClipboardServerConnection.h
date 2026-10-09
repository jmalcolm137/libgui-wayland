/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <Clipboard/ClipboardClientEndpoint.h>
#include <Clipboard/ClipboardServerEndpoint.h>

#include "ClipboardServerDefaultStub.h"

namespace LibWM {

// A minimal in-process ClipboardServer. It answers requests with "no data" so
// that widgets which read the clipboard on startup (e.g. LibGUI TextEditor) do
// not block. Real clipboard contents over wl_data_device are a later milestone.
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
    }

    virtual Messages::ClipboardServer::GetClipboardDataResponse get_clipboard_data() override
    {
        return Messages::ClipboardServer::GetClipboardDataResponse { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };
    }

    virtual void set_clipboard_data(Core::AnonymousBuffer const&, ByteString const&, HashMap<ByteString, ByteString> const&) override
    {
    }
};

}
