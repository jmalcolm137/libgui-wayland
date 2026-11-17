/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandClient.h"
#include <AK/ByteBuffer.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

namespace LibWM {

// --- wl_data_device / wl_data_offer / wl_data_source -------------------------

static void data_device_data_offer(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandClient*>(data)->on_data_offer(offer);
}

static void data_device_enter(void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer)
{
    static_cast<WaylandClient*>(data)->on_drag_enter(serial, offer, surface, { wl_fixed_to_int(x), wl_fixed_to_int(y) });
}

static void data_device_leave(void* data, wl_data_device*)
{
    static_cast<WaylandClient*>(data)->on_drag_leave();
}

static void data_device_motion(void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    static_cast<WaylandClient*>(data)->on_drag_motion({ wl_fixed_to_int(x), wl_fixed_to_int(y) });
}

static void data_device_drop(void* data, wl_data_device*)
{
    static_cast<WaylandClient*>(data)->on_drag_drop();
}

static void data_device_selection(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandClient*>(data)->on_selection(offer);
}

static wl_data_device_listener const s_data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void data_offer_offer(void* data, wl_data_offer* offer, char const* mime_type)
{
    static_cast<WaylandClient*>(data)->on_data_offer_mime(offer, mime_type);
}

static void data_offer_source_actions(void*, wl_data_offer*, uint32_t) { }
static void data_offer_action(void*, wl_data_offer*, uint32_t) { }

static wl_data_offer_listener const s_data_offer_listener = {
    .offer = data_offer_offer,
    .source_actions = data_offer_source_actions,
    .action = data_offer_action,
};

static void data_source_target(void*, wl_data_source*, char const*) { }

static void data_source_send(void* data, wl_data_source* source, char const* mime_type, int32_t fd)
{
    static_cast<WaylandClient*>(data)->on_source_send(source, mime_type, fd);
}

static void data_source_cancelled(void* data, wl_data_source* source)
{
    static_cast<WaylandClient*>(data)->on_source_cancelled(source);
}

static void data_source_dnd_drop_performed(void*, wl_data_source*) { }
static void data_source_dnd_finished(void*, wl_data_source*) { }
static void data_source_action(void*, wl_data_source*, uint32_t) { }

static wl_data_source_listener const s_data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_dnd_drop_performed,
    .dnd_finished = data_source_dnd_finished,
    .action = data_source_action,
};

// The drag source is separate from the clipboard source: it reports the drag
// outcome (drop performed / finished / cancelled) so the DragOperation can
// resolve, rather than only serving data.
static void drag_source_cancelled(void* data, wl_data_source* source)
{
    static_cast<WaylandClient*>(data)->on_drag_source_cancelled(source);
}
static void drag_source_drop_performed(void* data, wl_data_source*)
{
    static_cast<WaylandClient*>(data)->on_drag_source_drop_performed();
}
static void drag_source_finished(void* data, wl_data_source*)
{
    static_cast<WaylandClient*>(data)->on_drag_source_finished();
}
static void drag_source_action(void* data, wl_data_source*, uint32_t action)
{
    static_cast<WaylandClient*>(data)->on_drag_source_action(action);
}
static wl_data_source_listener const s_drag_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = drag_source_cancelled,
    .dnd_drop_performed = drag_source_drop_performed,
    .dnd_finished = drag_source_finished,
    .action = drag_source_action,
};

void WaylandClient::set_data_device_manager(wl_data_device_manager* manager)
{
    MutexLocker locker(m_mutex);
    m_data_device.manager = manager;
    maybe_create_data_device();
}

void WaylandClient::maybe_create_data_device()
{
    MutexLocker locker(m_mutex);
    if (m_data_device.device || !m_input_state.seat || !m_data_device.manager)
        return;
    m_data_device.device = wl_data_device_manager_get_data_device(m_data_device.manager, m_input_state.seat);
    dbgln("LibWM/Wayland: data device created: {}", m_data_device.device != nullptr);
    if (m_data_device.device)
        wl_data_device_add_listener(m_data_device.device, &s_data_device_listener, this);
}

ByteString WaylandClient::preferred_mime_for(wl_data_offer* offer) const
{
    MutexLocker locker(m_mutex);
    auto it = m_data_device.offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it == m_data_device.offer_mime_types.end() || it->value.is_empty())
        return {};
    auto const& mimes = it->value;
    for (auto const& candidate : { "text/plain;charset=utf-8"sv, "text/plain"sv, "UTF8_STRING"sv, "STRING"sv, "text/uri-list"sv, "image/png"sv }) {
        for (auto const& mime : mimes) {
            if (mime == candidate)
                return mime;
        }
    }
    // No known type: fall back to the source's first offer (e.g. image/png).
    return mimes.first();
}

void WaylandClient::on_data_offer(wl_data_offer* offer)
{
    MutexLocker locker(m_mutex);
    dbgln("LibWM/Wayland: data offer {}", static_cast<void*>(offer));
    m_data_device.pending_offers.append(offer);
    m_data_device.offer_mime_types.set(reinterpret_cast<u64>(offer), {});
    wl_data_offer_add_listener(offer, &s_data_offer_listener, this);
}

void WaylandClient::on_data_offer_mime(wl_data_offer* offer, char const* mime_type)
{
    MutexLocker locker(m_mutex);
    auto it = m_data_device.offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it != m_data_device.offer_mime_types.end())
        it->value.append(ByteString(mime_type));
}

void WaylandClient::on_selection(wl_data_offer* offer)
{
    MutexLocker locker(m_mutex);
    for (auto* pending : m_data_device.pending_offers) {
        // The drag offer belongs to the drag, not to the selection. Destroying it
        // here used to leave drag_offer dangling, and the drop/leave destroyed it
        // again -- a segfault inside wl_data_offer_destroy (a freed wl_proxy).
        if (pending == offer || pending == m_data_device.drag_offer)
            continue;
        m_data_device.offer_mime_types.remove(reinterpret_cast<u64>(pending));
        wl_data_offer_destroy(pending);
    }
    m_data_device.pending_offers.clear();
    if (offer)
        m_data_device.pending_offers.append(offer);
    m_data_device.current_offer = offer;

    ByteString mime;
    if (offer) {
        mime = preferred_mime_for(offer);
        // Serenity clients key on "text/plain"; normalize the charset variant.
        if (mime.starts_with("text/plain"sv))
            mime = ByteString("text/plain");
    }
    dbgln("LibWM/Wayland: selection changed (mime='{}')", mime);
    if (m_data_device.on_clipboard_changed)
        m_data_device.on_clipboard_changed(mime);
}

void WaylandClient::on_source_send(wl_data_source*, char const* mime_type, int fd)
{
    MutexLocker locker(m_mutex);
    auto write_data = [fd](ByteBuffer const& data) {
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = write(fd, data.data() + written, data.size() - written);
            if (n <= 0)
                break;
            written += static_cast<size_t>(n);
        }
    };
    // The source may be serving the clipboard selection or a drag payload.
    if (auto it = m_data_device.clipboard_offers.find(ByteString(mime_type)); it != m_data_device.clipboard_offers.end())
        write_data(it->value);
    else if (auto it = m_data_device.drag_payload.find(String::from_utf8(StringView { mime_type, strlen(mime_type) }).release_value_but_fixme_should_propagate_errors()); it != m_data_device.drag_payload.end())
        write_data(it->value);
    close(fd);
}

void WaylandClient::on_source_cancelled(wl_data_source* source)
{
    MutexLocker locker(m_mutex);
    if (source == m_data_device.selection_source)
        m_data_device.selection_source = nullptr;
    if (source)
        wl_data_source_destroy(source);
}

// --- explicit drag-and-drop --------------------------------------------------

// A shm wl_buffer over an anonymous buffer, for the drag icon. The pool can go
// away once the buffer exists, but the memory must outlive the drag (the
// compositor mmaps it), so the caller keeps the AnonymousBuffer alive.
static wl_buffer* create_icon_buffer(wl_shm* shm, Core::AnonymousBuffer& anon, Gfx::Bitmap const& bitmap)
{
    auto* pool = wl_shm_create_pool(shm, anon.fd(), static_cast<int>(anon.size()));
    if (!pool)
        return nullptr;
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, bitmap.width(), bitmap.height(), bitmap.pitch(), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buffer;
}

bool WaylandClient::start_drag(ByteString const&, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& drag_bitmap)
{
    MutexLocker locker(m_mutex);
    if (!m_data_device.device || !m_data_device.manager || !m_compositor || !m_shm)
        return false;
    auto* origin = find(m_input_state.pointer_window);
    if (!origin || !origin->surface)
        return false;

    m_data_device.drag_payload = mime_data;
    m_data_device.drag_dropped = false;

    m_data_device.drag_source = wl_data_device_manager_create_data_source(m_data_device.manager);
    if (!m_data_device.drag_source)
        return false;
    wl_data_source_add_listener(m_data_device.drag_source, &s_drag_source_listener, this);
    for (auto const& it : mime_data) {
        auto mime = it.key.to_byte_string();
        wl_data_source_offer(m_data_device.drag_source, mime.characters());
    }
    wl_data_source_set_actions(m_data_device.drag_source,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    // The drag icon (a plain surface carrying the drag bitmap), if one was given.
    if (m_data_device.drag_icon) {
        wl_surface_destroy(m_data_device.drag_icon);
        m_data_device.drag_icon = nullptr;
    }
    if (m_data_device.drag_icon_buffer) {
        wl_buffer_destroy(m_data_device.drag_icon_buffer);
        m_data_device.drag_icon_buffer = nullptr;
    }
    m_data_device.drag_icon_anon.clear();
    if (drag_bitmap.is_valid() && drag_bitmap.bitmap()) {
        auto const& bitmap = *drag_bitmap.bitmap();
        auto anon = Core::AnonymousBuffer::create_with_size(static_cast<size_t>(bitmap.pitch()) * static_cast<size_t>(bitmap.height()));
        if (!anon.is_error()) {
            auto buffer = anon.release_value();
            memcpy(buffer.data<u8>(), bitmap.scanline(0), static_cast<size_t>(bitmap.pitch()) * static_cast<size_t>(bitmap.height()));
            m_data_device.drag_icon = wl_compositor_create_surface(m_compositor);
            m_data_device.drag_icon_buffer = create_icon_buffer(m_shm, buffer, bitmap);
            if (m_data_device.drag_icon && m_data_device.drag_icon_buffer) {
                wl_surface_attach(m_data_device.drag_icon, m_data_device.drag_icon_buffer, 0, 0);
                wl_surface_damage_buffer(m_data_device.drag_icon, 0, 0, bitmap.width(), bitmap.height());
                wl_surface_commit(m_data_device.drag_icon);
                m_data_device.drag_icon_anon = move(buffer);
            }
        }
    }

    wl_data_device_start_drag(m_data_device.device, m_data_device.drag_source, origin->surface, m_data_device.drag_icon, m_input_state.last_serial);
    wl_display_flush(m_display);
    dbgln("LibWM/Wayland: started drag ({} representation(s), serial {})", mime_data.size(), m_input_state.last_serial);
    return true;
}

void WaylandClient::on_drag_source_cancelled(wl_data_source* source)
{
    MutexLocker locker(m_mutex);
    if (source == m_data_device.drag_source)
        m_data_device.drag_source = nullptr;
    wl_data_source_destroy(source);
    m_data_device.drag_payload.clear();
    if (m_input.drag_finished)
        m_input.drag_finished(false);
}

void WaylandClient::on_drag_source_drop_performed()
{
    MutexLocker locker(m_mutex);
    m_data_device.drag_dropped = true;
}

void WaylandClient::on_drag_source_finished()
{
    MutexLocker locker(m_mutex);
    bool accepted = m_data_device.drag_dropped && m_data_device.drag_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    if (m_data_device.drag_source) {
        wl_data_source_destroy(m_data_device.drag_source);
        m_data_device.drag_source = nullptr;
    }
    if (m_data_device.drag_icon) {
        wl_surface_destroy(m_data_device.drag_icon);
        m_data_device.drag_icon = nullptr;
    }
    if (m_data_device.drag_icon_buffer) {
        wl_buffer_destroy(m_data_device.drag_icon_buffer);
        m_data_device.drag_icon_buffer = nullptr;
    }
    m_data_device.drag_icon_anon.clear();
    m_data_device.drag_payload.clear();
    if (m_input.drag_finished)
        m_input.drag_finished(accepted);
}

Gfx::IntPoint WaylandClient::adjusted_drag_position() const
{
    MutexLocker locker(m_mutex);
    auto const* window_surface = const_cast<WaylandClient*>(this)->find(m_data_device.drag_window);
    int inset = window_surface ? window_surface->total_top_inset() : 0;
    return m_data_device.drag_position.translated(0, -inset);
}

ByteBuffer WaylandClient::receive_offer_data(wl_data_offer* offer, ByteString const& mime)
{
    if (!offer)
        return {};
    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0)
        return {};
    {
        MutexLocker locker(m_mutex);
        wl_data_offer_receive(offer, mime.characters(), fds[1]);
        close(fds[1]);
        wl_display_flush(m_display);
    }

    ByteBuffer buffer;
    u8 chunk[4096];
    for (;;) {
        ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n <= 0)
            break;
        if (auto appended = buffer.try_append(chunk, static_cast<size_t>(n)); appended.is_error())
            break;
    }
    close(fds[0]);
    return buffer;
}

void WaylandClient::on_drag_enter(u32 serial, wl_data_offer* offer, wl_surface* surface, Gfx::IntPoint position)
{
    MutexLocker locker(m_mutex);
    m_data_device.drag_offer = offer;
    m_data_device.drag_serial = serial;
    m_data_device.drag_window = window_id_for_surface(surface);
    m_data_device.drag_position = position;
    m_data_device.drag_mime = preferred_mime_for(offer);
    m_data_device.drag_received.clear();
    // Each drag starts undecided; the target widget accepts (or not).
    m_data_device.accepts_drag = false;
    // Fetch the offered data (the source writes it on demand). Skipped while this
    // process is itself the source: reading would block the same event loop that
    // would answer the request.
    if (offer && !m_data_device.drag_source) {
        if (auto it = m_data_device.offer_mime_types.find(reinterpret_cast<u64>(offer)); it != m_data_device.offer_mime_types.end()) {
            auto mimes = it->value;
            for (auto const& mime : mimes) {
                auto data = receive_offer_data(offer, mime);
                if (!data.is_empty())
                    m_data_device.drag_received.set(String::from_byte_string(mime).release_value_but_fixme_should_propagate_errors(), move(data));
            }
        }
    }
    // The widget decides whether it accepts; Application replies with
    // set_accepts_drag() after the dispatched event, which accepts the offer.
    if (m_data_device.accepts_drag && offer && !m_data_device.drag_mime.is_empty())
        wl_data_offer_accept(offer, serial, m_data_device.drag_mime.characters());
    if (m_data_device.drag_window >= 0 && m_input.drag_moved)
        m_input.drag_moved(m_data_device.drag_window, adjusted_drag_position(), m_input_state.pointer_buttons, current_modifiers(), m_data_device.drag_received);
}

void WaylandClient::on_drag_motion(Gfx::IntPoint position)
{
    MutexLocker locker(m_mutex);
    m_data_device.drag_position = position;
    if (m_data_device.drag_offer && m_data_device.accepts_drag && !m_data_device.drag_mime.is_empty())
        wl_data_offer_accept(m_data_device.drag_offer, m_data_device.drag_serial, m_data_device.drag_mime.characters());
    if (m_data_device.drag_window >= 0 && m_input.drag_moved)
        m_input.drag_moved(m_data_device.drag_window, adjusted_drag_position(), m_input_state.pointer_buttons, current_modifiers(), m_data_device.drag_received);
}

void WaylandClient::set_accepts_drag(bool accepts)
{
    MutexLocker locker(m_mutex);
    m_data_device.accepts_drag = accepts;
    if (accepts && m_data_device.drag_offer && !m_data_device.drag_mime.is_empty())
        wl_data_offer_accept(m_data_device.drag_offer, m_data_device.drag_serial, m_data_device.drag_mime.characters());
}

void WaylandClient::on_drag_drop()
{
    MutexLocker locker(m_mutex);
    if (m_data_device.drag_window >= 0 && m_input.drag_dropped)
        m_input.drag_dropped(m_data_device.drag_window, adjusted_drag_position(), m_input_state.pointer_buttons, current_modifiers(), m_data_device.drag_received);
    if (m_data_device.drag_offer) {
        wl_data_offer_finish(m_data_device.drag_offer);
        forget_offer(m_data_device.drag_offer);
        wl_data_offer_destroy(m_data_device.drag_offer);
        m_data_device.drag_offer = nullptr;
    }
    m_data_device.drag_window = -1;
    m_data_device.drag_received.clear();
}

void WaylandClient::on_drag_leave()
{
    MutexLocker locker(m_mutex);
    if (m_data_device.drag_offer) {
        forget_offer(m_data_device.drag_offer);
        wl_data_offer_destroy(m_data_device.drag_offer);
        m_data_device.drag_offer = nullptr;
    }
    m_data_device.drag_window = -1;
    m_data_device.drag_received.clear();
}

// An offer we are about to destroy must not be reachable from the selection state
// any more (the drag and the selection share pending_offers / offer_mime_types).
void WaylandClient::forget_offer(wl_data_offer* offer)
{
    MutexLocker locker(m_mutex);
    m_data_device.offer_mime_types.remove(reinterpret_cast<u64>(offer));
    if (m_data_device.current_offer == offer)
        m_data_device.current_offer = nullptr;
    for (size_t i = m_data_device.pending_offers.size(); i > 0; --i) {
        if (m_data_device.pending_offers[i - 1] == offer)
            m_data_device.pending_offers.remove(i - 1);
    }
}

// --- clipboard ---------------------------------------------------------------

Vector<ByteString> WaylandClient::clipboard_mime_types() const
{
    MutexLocker locker(m_mutex);
    // If we own the selection, the offers are the canonical list: the data-source
    // round trip would block the same event loop that has to answer it.
    if (m_data_device.selection_source && !m_data_device.clipboard_offers.is_empty()) {
        Vector<ByteString> mimes;
        mimes.ensure_capacity(m_data_device.clipboard_offers.size());
        for (auto const& it : m_data_device.clipboard_offers)
            mimes.append(ByteString { it.key.view() });
        return mimes;
    }

    if (!m_data_device.current_offer)
        return {};

    if (auto it = m_data_device.offer_mime_types.find(reinterpret_cast<u64>(m_data_device.current_offer)); it != m_data_device.offer_mime_types.end()) {
        Vector<ByteString> mimes;
        mimes.ensure_capacity(it->value.size());
        for (auto const& mime : it->value)
            mimes.append(ByteString { mime.view() });
        return mimes;
    }
    return {};
}

ErrorOr<ByteBuffer> WaylandClient::read_clipboard_for_mime(ByteString const& wanted_mime, ByteString& out_mime_type)
{
    out_mime_type = {};
    if (wanted_mime.is_empty())
        return Error::from_string_literal("LibWM: no MIME type requested");

    int fds[2] = { -1, -1 };
    {
        MutexLocker locker(m_mutex);
        // If we are the clipboard owner, serve the stored offers directly.
        if (m_data_device.selection_source && !m_data_device.clipboard_offers.is_empty()) {
            if (auto it = m_data_device.clipboard_offers.find(wanted_mime); it != m_data_device.clipboard_offers.end()) {
                out_mime_type = it->key;
                return ByteBuffer::copy(it->value);
            }

            // text/plain is offered under a few aliases; accept any of them.
            if (wanted_mime == "text/plain"sv || wanted_mime.starts_with("text/plain;"sv)) {
                for (auto const& alias : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "STRING"sv }) {
                    if (auto it = m_data_device.clipboard_offers.find(ByteString(alias)); it != m_data_device.clipboard_offers.end()) {
                        out_mime_type = ByteString("text/plain");
                        return ByteBuffer::copy(it->value);
                    }
                }
            }

            return Error::from_string_literal("LibWM: selection does not offer the requested MIME type");
        }

        if (!m_data_device.current_offer || !m_display)
            return Error::from_string_literal("LibWM: no clipboard selection");

        if (pipe(fds) != 0)
            return Error::from_errno(errno);

        wl_data_offer_receive(m_data_device.current_offer, wanted_mime.characters(), fds[1]);
        wl_display_flush(m_display);
        close(fds[1]);
    }

    // Read from the pipe outside `m_mutex` so the main thread can keep
    // dispatching Wayland events while the selection owner writes.
    ByteBuffer data;
    char buffer[4096];
    for (;;) {
        struct pollfd pfd {
            .fd = fds[0],
            .events = POLLIN,
            .revents = 0,
        };
        int rc = poll(&pfd, 1, 2000);
        if (rc <= 0)
            break;
        ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n <= 0)
            break;
        if (auto appended = data.try_append(buffer, static_cast<size_t>(n)); appended.is_error()) {
            close(fds[0]);
            return appended.release_error();
        }
    }
    close(fds[0]);

    auto mime = wanted_mime;
    if (mime.starts_with("text/plain"sv))
        mime = ByteString("text/plain");
    out_mime_type = move(mime);
    return data;
}

ErrorOr<ByteBuffer> WaylandClient::read_clipboard(ByteString& out_mime_type)
{
    out_mime_type = {};

    ByteString mime;
    {
        MutexLocker locker(m_mutex);
        if (m_data_device.selection_source && !m_data_device.clipboard_offers.is_empty()) {
            for (auto const& preferred : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "text/uri-list"sv, "image/png"sv, "image/x-serenityos"sv }) {
                if (m_data_device.clipboard_offers.contains(ByteString(preferred))) {
                    mime = ByteString(preferred);
                    break;
                }
            }
            if (mime.is_empty())
                mime = m_data_device.clipboard_offers.begin()->key;
        } else {
            if (!m_data_device.current_offer)
                return Error::from_string_literal("LibWM: no clipboard selection");
            mime = preferred_mime_for(m_data_device.current_offer);
            if (mime.is_empty())
                return Error::from_string_literal("LibWM: selection offers no usable type");
        }
    }

    return read_clipboard_for_mime(mime, out_mime_type);
}

void WaylandClient::write_clipboard(HashMap<ByteString, ByteBuffer> offers)
{
    MutexLocker locker(m_mutex);
    m_data_device.clipboard_offers = move(offers);

    if (!m_data_device.manager || !m_data_device.device) {
        dbgln("LibWM/Wayland: cannot set clipboard (no data device)");
        return;
    }

    if (m_data_device.selection_source) {
        wl_data_source_destroy(m_data_device.selection_source);
        m_data_device.selection_source = nullptr;
    }

    if (m_data_device.clipboard_offers.is_empty()) {
        wl_data_device_set_selection(m_data_device.device, nullptr, m_input_state.last_serial);
        wl_display_flush(m_display);
        return;
    }

    m_data_device.selection_source = wl_data_device_manager_create_data_source(m_data_device.manager);
    wl_data_source_add_listener(m_data_device.selection_source, &s_data_source_listener, this);
    for (auto const& it : m_data_device.clipboard_offers)
        wl_data_source_offer(m_data_device.selection_source, it.key.characters());
    wl_data_device_set_selection(m_data_device.device, m_data_device.selection_source, m_input_state.last_serial);
    wl_display_flush(m_display);
    dbgln("LibWM/Wayland: set clipboard ({} representation(s), serial {})", m_data_device.clipboard_offers.size(), m_input_state.last_serial);
}

}
