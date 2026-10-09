/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WaylandClient.h"
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Socket.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibIPC/Connection.h>
#include <Clipboard/ClipboardClientEndpoint.h>
#include <Clipboard/ClipboardServerEndpoint.h>
#include <string.h>

#include "ClipboardServerDefaultStub.h"

namespace LibWM {

// Serenity image clipboards carry raw pixels under "image/x-serenityos" with
// width/height/scale/format/pitch metadata; Wayland's interoperable image type
// is "image/png". These two helpers translate between them with LibGfx.

inline ErrorOr<ByteBuffer> serenity_image_to_png(ReadonlyBytes raw, HashMap<ByteString, ByteString> const& metadata)
{
    auto number = [&](StringView key) -> Optional<unsigned> {
        auto value = metadata.get(ByteString(key));
        if (!value.has_value())
            return {};
        return value.value().to_number<unsigned>();
    };

    auto width = number("width"sv);
    auto height = number("height"sv);
    auto scale = number("scale"sv);
    auto pitch = number("pitch"sv);
    auto format = number("format"sv);
    if (!width.has_value() || !height.has_value() || !scale.has_value() || !pitch.has_value() || !format.has_value())
        return Error::from_string_literal("image/x-serenityos is missing metadata");
    if (!Gfx::is_valid_bitmap_format(format.value()))
        return Error::from_string_literal("image/x-serenityos has an invalid bitmap format");

    auto bitmap = TRY(Gfx::Bitmap::create_wrapper(
        static_cast<Gfx::BitmapFormat>(format.value()),
        { static_cast<int>(width.value()), static_cast<int>(height.value()) },
        static_cast<int>(scale.value()),
        pitch.value(),
        const_cast<u8*>(raw.data())));
    return Gfx::PNGWriter::encode(*bitmap);
}

inline ErrorOr<ByteBuffer> png_to_serenity_image(ReadonlyBytes png, HashMap<ByteString, ByteString>& out_metadata)
{
    auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(png, "image/png"sv));
    if (!decoder)
        return Error::from_string_literal("PNG decode failed");
    auto frame = TRY(decoder->frame(0));
    if (!frame.image)
        return Error::from_string_literal("PNG has no image frame");

    auto const& bitmap = *frame.image;
    out_metadata.set(ByteString("width"), ByteString::number(bitmap.width()));
    out_metadata.set(ByteString("height"), ByteString::number(bitmap.height()));
    out_metadata.set(ByteString("scale"), ByteString::number(bitmap.scale()));
    out_metadata.set(ByteString("format"), ByteString::number(static_cast<int>(bitmap.format())));
    out_metadata.set(ByteString("pitch"), ByteString::number(bitmap.pitch()));
    return ByteBuffer::copy({ bitmap.scanline(0), bitmap.size_in_bytes() });
}

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
        ByteString wayland_mime;
        auto received = WaylandClient::the().read_clipboard(wayland_mime);
        if (received.is_error() || received.value().is_empty())
            return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };

        auto bytes = received.release_value();
        HashMap<ByteString, ByteString> metadata;
        auto serenity_mime = wayland_mime;

        // text/plain is normalized by read_clipboard(); text/uri-list passes
        // through; image/png is transcoded to Serenity's raw image format.
        if (wayland_mime == "image/png"sv) {
            auto image = png_to_serenity_image(bytes.bytes(), metadata);
            if (image.is_error()) {
                dbgln("LibWM: clipboard PNG decode failed: {}", image.error());
                return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };
            }
            bytes = image.release_value();
            serenity_mime = ByteString("image/x-serenityos");
        }

        auto buffer = Core::AnonymousBuffer::create_with_size(bytes.size());
        if (buffer.is_error())
            return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };
        memcpy(buffer.value().data<void>(), bytes.data(), bytes.size());
        return { buffer.release_value(), move(serenity_mime), move(metadata) };
    }

    virtual void set_clipboard_data(Core::AnonymousBuffer const& data, ByteString const& mime_type, HashMap<ByteString, ByteString> const& metadata) override
    {
        HashMap<ByteString, ByteBuffer> offers;

        if (data.is_valid() && data.size() > 0) {
            ReadonlyBytes bytes { data.data<u8>(), data.size() };

            if (mime_type == "image/x-serenityos"sv) {
                if (auto png = serenity_image_to_png(bytes, metadata); !png.is_error())
                    offers.set(ByteString("image/png"), png.release_value());
                else
                    dbgln("LibWM: could not transcode image/x-serenityos to PNG: {}", png.error());
                // Also offer the raw form, so LibWM-to-LibWM copies stay lossless.
                if (auto raw = ByteBuffer::copy(bytes); !raw.is_error())
                    offers.set(ByteString("image/x-serenityos"), raw.release_value());
            } else if (mime_type == "text/plain"sv) {
                for (auto const& alias : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "STRING"sv }) {
                    if (auto copy = ByteBuffer::copy(bytes); !copy.is_error())
                        offers.set(ByteString(alias), copy.release_value());
                }
            } else {
                // text/uri-list and any other type pass through unchanged.
                if (auto copy = ByteBuffer::copy(bytes); !copy.is_error())
                    offers.set(mime_type, copy.release_value());
            }
        }

        WaylandClient::the().write_clipboard(move(offers));
    }
};

}
