/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardServerConnection.h"
#include "MainThreadInvoker.h"
#include "WaylandClient.h"
#include "WaylandDataDevice.h"
#include <AK/Optional.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <string.h>

namespace LibWM {

ErrorOr<ByteBuffer> serenity_image_to_png(ReadonlyBytes raw, HashMap<ByteString, ByteString> const& metadata)
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

ErrorOr<ByteBuffer> png_to_serenity_image(ReadonlyBytes png, HashMap<ByteString, ByteString>& out_metadata)
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

ClipboardServerConnection::ClipboardServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::Connection<ClipboardServerEndpoint, ClipboardClientEndpoint>(*this, move(socket))
    , ClipboardClientEndpoint::Proxy<ClipboardServerEndpoint>(*this, {})
{
    // The selection changes on the main thread (Wayland dispatch), but this
    // connection and its socket belong to the LibWM server thread. The connection
    // lives for the whole process (LibWM leaks its slot), so `this` stays valid.
    WaylandClient::the().data_device().set_clipboard_changed_callback([this](ByteString const& mime_type) {
        if (MainThreadInvoker::on_server_thread()) {
            async_clipboard_data_changed(mime_type);
            return;
        }
        MainThreadInvoker::post_to_server([this, mime_type = ByteString { mime_type.view() }] {
            async_clipboard_data_changed(mime_type);
        });
    });
}

bool ClipboardServerConnection::is_plain_text(StringView mime_type)
{
    return mime_type == "text/plain"sv || mime_type.starts_with("text/plain;"sv) || mime_type == "UTF8_STRING"sv || mime_type == "STRING"sv;
}

bool ClipboardServerConnection::is_html(StringView mime_type)
{
    return mime_type == "text/html"sv || mime_type.starts_with("text/html;"sv);
}

Vector<GUI::ClipboardEntry> ClipboardServerConnection::current_entries() const
{
    auto& client = WaylandClient::the();
    if (client.data_device().owns_clipboard() && !m_entries.is_empty())
        return m_entries;

    auto mimes = client.data_device().clipboard_mime_types();
    Vector<GUI::ClipboardEntry> entries;
    if (mimes.is_empty())
        return entries;

    auto find = [&](auto predicate) -> Optional<ByteString> {
        for (auto const& mime : mimes) {
            if (predicate(mime))
                return mime;
        }
        return {};
    };

    auto fetch = [&](ByteString const& mime, ByteString canonical_mime) {
        ByteString actual_mime;
        auto data = client.data_device().read_clipboard_for_mime(mime, actual_mime);
        if (data.is_error())
            return;
        entries.append({ data.release_value(), move(canonical_mime), {} });
    };

    if (auto mime = find([](auto const& candidate) { return is_plain_text(candidate); }); mime.has_value())
        fetch(*mime, ByteString("text/plain"));
    if (auto mime = find([](auto const& candidate) { return is_html(candidate); }); mime.has_value())
        fetch(*mime, ByteString("text/html"));
    if (auto mime = find([](auto const& candidate) { return candidate == "text/uri-list"sv; }); mime.has_value())
        fetch(*mime, ByteString("text/uri-list"));

    // Prefer PNG over the raw Serenity format: the transfer carries no
    // metadata, so decoding the PNG is the only way to restore it.
    if (auto mime = find([](auto const& candidate) { return candidate == "image/png"sv; }); mime.has_value()) {
        ByteString actual_mime;
        auto png = client.data_device().read_clipboard_for_mime(*mime, actual_mime);
        if (!png.is_error()) {
            HashMap<ByteString, ByteString> metadata;
            auto raw = png_to_serenity_image(png.value().bytes(), metadata);
            if (!raw.is_error())
                entries.append({ raw.release_value(), ByteString("image/x-serenityos"), move(metadata) });
            else
                entries.append({ png.release_value(), ByteString("image/png"), {} });
        }
    } else if (auto mime = find([](auto const& candidate) { return candidate == "image/x-serenityos"sv; }); mime.has_value()) {
        fetch(*mime, ByteString("image/x-serenityos"));
    }

    for (auto const& mime : mimes) {
        if (is_plain_text(mime) || is_html(mime) || mime == "text/uri-list"sv || mime.starts_with("image/"sv))
            continue;
        fetch(mime, mime);
    }

    return entries;
}

GUI::ClipboardEntry const* ClipboardServerConnection::pick_primary(Vector<GUI::ClipboardEntry> const& entries)
{
    GUI::ClipboardEntry const* first = nullptr;

    for (auto const& entry : entries) {
        if (entry.data.is_empty())
            continue;
        if (!first)
            first = &entry;
        if (is_plain_text(entry.mime_type))
            return &entry;
    }
    for (auto const& entry : entries) {
        if (!entry.data.is_empty() && is_html(entry.mime_type))
            return &entry;
    }
    for (auto const& entry : entries) {
        if (!entry.data.is_empty() && entry.mime_type == "text/uri-list"sv)
            return &entry;
    }
    for (auto const& entry : entries) {
        if (!entry.data.is_empty() && entry.mime_type.starts_with("image/"sv))
            return &entry;
    }
    return first;
}

Messages::ClipboardServer::GetClipboardDataResponse ClipboardServerConnection::get_clipboard_data()
{
    auto entries = current_entries();
    auto const* primary = pick_primary(entries);
    if (!primary)
        return { Core::AnonymousBuffer {}, ByteString {}, HashMap<ByteString, ByteString> {} };

    if (primary->data.is_empty())
        return { Core::AnonymousBuffer {}, primary->mime_type, primary->metadata };

    auto buffer = Core::AnonymousBuffer::create_with_size(primary->data.size());
    if (buffer.is_error())
        return { Core::AnonymousBuffer {}, primary->mime_type, primary->metadata };
    memcpy(buffer.value().data<void>(), primary->data.data(), primary->data.size());
    return { buffer.release_value(), primary->mime_type, primary->metadata };
}

Messages::ClipboardServer::GetClipboardEntriesResponse ClipboardServerConnection::get_clipboard_entries()
{
    return { current_entries() };
}

void ClipboardServerConnection::set_clipboard_data(Core::AnonymousBuffer const& data, ByteString const& mime_type, HashMap<ByteString, ByteString> const& metadata)
{
    ByteBuffer bytes;
    if (data.is_valid()) {
        auto copied = ByteBuffer::copy(data.data<void>(), data.size());
        if (copied.is_error())
            return;
        bytes = copied.release_value();
    }

    Vector<GUI::ClipboardEntry> entries;
    entries.append({ move(bytes), mime_type, metadata });
    set_clipboard_entries(entries);
}

void ClipboardServerConnection::set_clipboard_entries(Vector<GUI::ClipboardEntry> const& entries)
{
    m_entries = entries;

    HashMap<ByteString, ByteBuffer> offers;

    auto copy = [](ByteBuffer const& data) -> Optional<ByteBuffer> {
        auto copied = ByteBuffer::copy(data.bytes());
        if (copied.is_error())
            return {};
        return copied.release_value();
    };

    for (auto const& entry : entries) {
        if (entry.data.is_empty())
            continue;

        if (entry.mime_type == "image/x-serenityos"sv) {
            if (auto png = serenity_image_to_png(entry.data.bytes(), entry.metadata); !png.is_error())
                offers.set(ByteString("image/png"), png.release_value());
            else
                dbgln("LibWM: could not transcode image/x-serenityos to PNG: {}", png.error());
            // Also offer the raw form, so LibWM-to-LibWM copies stay lossless.
            if (auto raw = copy(entry.data); raw.has_value())
                offers.set(ByteString("image/x-serenityos"), raw.release_value());
        } else if (is_plain_text(entry.mime_type)) {
            for (auto const& alias : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "STRING"sv }) {
                if (auto raw = copy(entry.data); raw.has_value())
                    offers.set(ByteString(alias), raw.release_value());
            }
        } else if (is_html(entry.mime_type)) {
            for (auto const& alias : { "text/html"sv, "text/html;charset=utf-8"sv }) {
                if (auto raw = copy(entry.data); raw.has_value())
                    offers.set(ByteString(alias), raw.release_value());
            }
        } else {
            // text/uri-list and any other type pass through unchanged.
            if (auto raw = copy(entry.data); raw.has_value())
                offers.set(entry.mime_type, raw.release_value());
        }
    }

    WaylandClient::the().data_device().write_clipboard(move(offers));
}

}
