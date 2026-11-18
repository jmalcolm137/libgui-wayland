/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Socket.h>
#include <LibGUI/ClipboardEntry.h>
#include <LibIPC/Connection.h>
#include <Clipboard/ClipboardClientEndpoint.h>
#include <Clipboard/ClipboardServerEndpoint.h>

#include "ClipboardServerDefaultStub.h"

namespace LibWM {

// Serenity image clipboards carry raw pixels under "image/x-serenityos" with
// width/height/scale/format/pitch metadata; Wayland's interoperable image type
// is "image/png". These two helpers translate between them with LibGfx.
ErrorOr<ByteBuffer> serenity_image_to_png(ReadonlyBytes raw, HashMap<ByteString, ByteString> const& metadata);
ErrorOr<ByteBuffer> png_to_serenity_image(ReadonlyBytes png, HashMap<ByteString, ByteString>& out_metadata);

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
    explicit ClipboardServerConnection(NonnullOwnPtr<Core::LocalSocket> socket);

    static bool is_plain_text(StringView mime_type);
    static bool is_html(StringView mime_type);

    // The clipboard as a list of representations. While we own the selection this is what we published; otherwise the
    // offers are read back from Wayland, one canonical entry per type.
    Vector<GUI::ClipboardEntry> current_entries() const;
    static GUI::ClipboardEntry const* pick_primary(Vector<GUI::ClipboardEntry> const& entries);

    virtual Messages::ClipboardServer::GetClipboardDataResponse get_clipboard_data() override;
    virtual Messages::ClipboardServer::GetClipboardEntriesResponse get_clipboard_entries() override;
    virtual void set_clipboard_data(Core::AnonymousBuffer const& data, ByteString const& mime_type, HashMap<ByteString, ByteString> const& metadata) override;
    virtual void set_clipboard_entries(Vector<GUI::ClipboardEntry> const& entries) override;

    Vector<GUI::ClipboardEntry> m_entries;
};

}
