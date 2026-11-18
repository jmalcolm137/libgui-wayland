/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandDataDevice.h"
#include "WaylandClient.h"
#include <AK/ByteBuffer.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

namespace LibWM {

using MutexLocker = WaylandClient::MutexLocker;

// --- wl_data_device / wl_data_offer / wl_data_source -------------------------

static void data_device_data_offer(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandDataDevice*>(data)->on_data_offer(offer);
}

static void data_device_enter(void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_enter(serial, offer, surface, { wl_fixed_to_int(x), wl_fixed_to_int(y) });
}

static void data_device_leave(void* data, wl_data_device*)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_leave();
}

static void data_device_motion(void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_motion({ wl_fixed_to_int(x), wl_fixed_to_int(y) });
}

static void data_device_drop(void* data, wl_data_device*)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_drop();
}

static void data_device_selection(void* data, wl_data_device*, wl_data_offer* offer)
{
    static_cast<WaylandDataDevice*>(data)->on_selection(offer);
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
    static_cast<WaylandDataDevice*>(data)->on_data_offer_mime(offer, mime_type);
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
    static_cast<WaylandDataDevice*>(data)->on_source_send(source, mime_type, fd);
}

static void data_source_cancelled(void* data, wl_data_source* source)
{
    static_cast<WaylandDataDevice*>(data)->on_source_cancelled(source);
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
    static_cast<WaylandDataDevice*>(data)->on_drag_source_cancelled(source);
}
static void drag_source_drop_performed(void* data, wl_data_source*)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_source_drop_performed();
}
static void drag_source_finished(void* data, wl_data_source*)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_source_finished();
}
static void drag_source_action(void* data, wl_data_source*, uint32_t action)
{
    static_cast<WaylandDataDevice*>(data)->on_drag_source_action(action);
}
static wl_data_source_listener const s_drag_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = drag_source_cancelled,
    .dnd_drop_performed = drag_source_drop_performed,
    .dnd_finished = drag_source_finished,
    .action = drag_source_action,
};

WaylandDataDevice::WaylandDataDevice(WaylandClient& client)
    : m_client(client)
{
}

void WaylandDataDevice::set_clipboard_changed_callback(Function<void(ByteString const&)> callback)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.on_clipboard_changed = move(callback);
}

bool WaylandDataDevice::owns_clipboard() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_state.selection_source && !m_state.clipboard_offers.is_empty();
}

void WaylandDataDevice::on_drag_source_action(u32 action)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.drag_action = action;
}

void WaylandDataDevice::bind(wl_data_device_manager* manager)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.manager = manager;
    maybe_create_data_device();
}

void WaylandDataDevice::maybe_create_data_device()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.device || !m_client.m_input_state.seat || !m_state.manager)
        return;
    m_state.device = wl_data_device_manager_get_data_device(m_state.manager, m_client.m_input_state.seat);
    dbgln("LibWM/Wayland: data device created: {}", m_state.device != nullptr);
    if (m_state.device)
        wl_data_device_add_listener(m_state.device, &s_data_device_listener, this);
}

ByteString WaylandDataDevice::preferred_mime_for(wl_data_offer* offer) const
{
    MutexLocker locker(m_client.m_mutex);
    auto it = m_state.offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it == m_state.offer_mime_types.end() || it->value.is_empty())
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

void WaylandDataDevice::on_data_offer(wl_data_offer* offer)
{
    MutexLocker locker(m_client.m_mutex);
    dbgln("LibWM/Wayland: data offer {}", static_cast<void*>(offer));
    m_state.pending_offers.append(offer);
    m_state.offer_mime_types.set(reinterpret_cast<u64>(offer), {});
    wl_data_offer_add_listener(offer, &s_data_offer_listener, this);
}

void WaylandDataDevice::on_data_offer_mime(wl_data_offer* offer, char const* mime_type)
{
    MutexLocker locker(m_client.m_mutex);
    auto it = m_state.offer_mime_types.find(reinterpret_cast<u64>(offer));
    if (it != m_state.offer_mime_types.end())
        it->value.append(ByteString(mime_type));
}

void WaylandDataDevice::on_selection(wl_data_offer* offer)
{
    MutexLocker locker(m_client.m_mutex);
    for (auto* pending : m_state.pending_offers) {
        // The drag offer belongs to the drag, not to the selection. Destroying it
        // here used to leave drag_offer dangling, and the drop/leave destroyed it
        // again -- a segfault inside wl_data_offer_destroy (a freed wl_proxy).
        if (pending == offer || pending == m_state.drag_offer)
            continue;
        m_state.offer_mime_types.remove(reinterpret_cast<u64>(pending));
        wl_data_offer_destroy(pending);
    }
    m_state.pending_offers.clear();
    if (offer)
        m_state.pending_offers.append(offer);
    m_state.current_offer = offer;

    ByteString mime;
    if (offer) {
        mime = preferred_mime_for(offer);
        // Serenity clients key on "text/plain"; normalize the charset variant.
        if (mime.starts_with("text/plain"sv))
            mime = ByteString("text/plain");
    }
    dbgln("LibWM/Wayland: selection changed (mime='{}')", mime);
    if (m_state.on_clipboard_changed)
        m_state.on_clipboard_changed(mime);
}

void WaylandDataDevice::on_source_send(wl_data_source*, char const* mime_type, int fd)
{
    MutexLocker locker(m_client.m_mutex);
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
    if (auto it = m_state.clipboard_offers.find(ByteString(mime_type)); it != m_state.clipboard_offers.end())
        write_data(it->value);
    else if (auto it = m_state.drag_payload.find(String::from_utf8(StringView { mime_type, strlen(mime_type) }).release_value_but_fixme_should_propagate_errors()); it != m_state.drag_payload.end())
        write_data(it->value);
    close(fd);
}

void WaylandDataDevice::on_source_cancelled(wl_data_source* source)
{
    MutexLocker locker(m_client.m_mutex);
    if (source == m_state.selection_source)
        m_state.selection_source = nullptr;
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

bool WaylandDataDevice::start_drag(ByteString const&, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& drag_bitmap)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_state.device || !m_state.manager || !m_client.m_compositor || !m_client.m_shm)
        return false;
    auto* origin = m_client.find(m_client.m_input_state.pointer_window);
    if (!origin || !origin->surface)
        return false;

    m_state.drag_payload = mime_data;
    m_state.drag_dropped = false;

    m_state.drag_source = wl_data_device_manager_create_data_source(m_state.manager);
    if (!m_state.drag_source)
        return false;
    wl_data_source_add_listener(m_state.drag_source, &s_drag_source_listener, this);
    for (auto const& it : mime_data) {
        auto mime = it.key.to_byte_string();
        wl_data_source_offer(m_state.drag_source, mime.characters());
    }
    wl_data_source_set_actions(m_state.drag_source,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    // The drag icon (a plain surface carrying the drag bitmap), if one was given.
    if (m_state.drag_icon) {
        wl_surface_destroy(m_state.drag_icon);
        m_state.drag_icon = nullptr;
    }
    if (m_state.drag_icon_buffer) {
        wl_buffer_destroy(m_state.drag_icon_buffer);
        m_state.drag_icon_buffer = nullptr;
    }
    m_state.drag_icon_anon.clear();
    if (drag_bitmap.is_valid() && drag_bitmap.bitmap()) {
        auto const& bitmap = *drag_bitmap.bitmap();
        auto anon = Core::AnonymousBuffer::create_with_size(static_cast<size_t>(bitmap.pitch()) * static_cast<size_t>(bitmap.height()));
        if (!anon.is_error()) {
            auto buffer = anon.release_value();
            memcpy(buffer.data<u8>(), bitmap.scanline(0), static_cast<size_t>(bitmap.pitch()) * static_cast<size_t>(bitmap.height()));
            m_state.drag_icon = wl_compositor_create_surface(m_client.m_compositor);
            m_state.drag_icon_buffer = create_icon_buffer(m_client.m_shm, buffer, bitmap);
            if (m_state.drag_icon && m_state.drag_icon_buffer) {
                wl_surface_attach(m_state.drag_icon, m_state.drag_icon_buffer, 0, 0);
                wl_surface_damage_buffer(m_state.drag_icon, 0, 0, bitmap.width(), bitmap.height());
                wl_surface_commit(m_state.drag_icon);
                m_state.drag_icon_anon = move(buffer);
            }
        }
    }

    wl_data_device_start_drag(m_state.device, m_state.drag_source, origin->surface, m_state.drag_icon, m_client.m_input_state.last_serial);
    wl_display_flush(m_client.m_display);
    dbgln("LibWM/Wayland: started drag ({} representation(s), serial {})", mime_data.size(), m_client.m_input_state.last_serial);
    return true;
}

void WaylandDataDevice::on_drag_source_cancelled(wl_data_source* source)
{
    MutexLocker locker(m_client.m_mutex);
    if (source == m_state.drag_source)
        m_state.drag_source = nullptr;
    wl_data_source_destroy(source);
    m_state.drag_payload.clear();
    if (m_client.m_input.drag_finished)
        m_client.m_input.drag_finished(false);
}

void WaylandDataDevice::on_drag_source_drop_performed()
{
    MutexLocker locker(m_client.m_mutex);
    m_state.drag_dropped = true;
}

void WaylandDataDevice::on_drag_source_finished()
{
    MutexLocker locker(m_client.m_mutex);
    bool accepted = m_state.drag_dropped && m_state.drag_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    if (m_state.drag_source) {
        wl_data_source_destroy(m_state.drag_source);
        m_state.drag_source = nullptr;
    }
    if (m_state.drag_icon) {
        wl_surface_destroy(m_state.drag_icon);
        m_state.drag_icon = nullptr;
    }
    if (m_state.drag_icon_buffer) {
        wl_buffer_destroy(m_state.drag_icon_buffer);
        m_state.drag_icon_buffer = nullptr;
    }
    m_state.drag_icon_anon.clear();
    m_state.drag_payload.clear();
    if (m_client.m_input.drag_finished)
        m_client.m_input.drag_finished(accepted);
}

Gfx::IntPoint WaylandDataDevice::adjusted_drag_position() const
{
    MutexLocker locker(m_client.m_mutex);
    auto const* window_surface = m_client.find(m_state.drag_window);
    int inset = window_surface ? window_surface->total_top_inset() : 0;
    return m_state.drag_position.translated(0, -inset);
}

ByteBuffer WaylandDataDevice::receive_offer_data(wl_data_offer* offer, ByteString const& mime)
{
    if (!offer)
        return {};
    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0)
        return {};
    {
        MutexLocker locker(m_client.m_mutex);
        wl_data_offer_receive(offer, mime.characters(), fds[1]);
        close(fds[1]);
        wl_display_flush(m_client.m_display);
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

void WaylandDataDevice::on_drag_enter(u32 serial, wl_data_offer* offer, wl_surface* surface, Gfx::IntPoint position)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.drag_offer = offer;
    m_state.drag_serial = serial;
    m_state.drag_window = m_client.window_id_for_surface(surface);
    m_state.drag_position = position;
    m_state.drag_mime = preferred_mime_for(offer);
    m_state.drag_received.clear();
    // Each drag starts undecided; the target widget accepts (or not).
    m_state.accepts_drag = false;
    // Fetch the offered data (the source writes it on demand). Skipped while this
    // process is itself the source: reading would block the same event loop that
    // would answer the request.
    if (offer && !m_state.drag_source) {
        if (auto it = m_state.offer_mime_types.find(reinterpret_cast<u64>(offer)); it != m_state.offer_mime_types.end()) {
            auto mimes = it->value;
            for (auto const& mime : mimes) {
                auto data = receive_offer_data(offer, mime);
                if (!data.is_empty())
                    m_state.drag_received.set(String::from_byte_string(mime).release_value_but_fixme_should_propagate_errors(), move(data));
            }
        }
    }
    // The widget decides whether it accepts; Application replies with
    // set_accepts_drag() after the dispatched event, which accepts the offer.
    if (m_state.accepts_drag && offer && !m_state.drag_mime.is_empty())
        wl_data_offer_accept(offer, serial, m_state.drag_mime.characters());
    if (m_state.drag_window >= 0 && m_client.m_input.drag_moved)
        m_client.m_input.drag_moved(m_state.drag_window, adjusted_drag_position(), m_client.m_input_state.pointer_buttons, m_client.current_modifiers(), m_state.drag_received);
}

void WaylandDataDevice::on_drag_motion(Gfx::IntPoint position)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.drag_position = position;
    if (m_state.drag_offer && m_state.accepts_drag && !m_state.drag_mime.is_empty())
        wl_data_offer_accept(m_state.drag_offer, m_state.drag_serial, m_state.drag_mime.characters());
    if (m_state.drag_window >= 0 && m_client.m_input.drag_moved)
        m_client.m_input.drag_moved(m_state.drag_window, adjusted_drag_position(), m_client.m_input_state.pointer_buttons, m_client.current_modifiers(), m_state.drag_received);
}

void WaylandDataDevice::set_accepts_drag(bool accepts)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.accepts_drag = accepts;
    if (accepts && m_state.drag_offer && !m_state.drag_mime.is_empty())
        wl_data_offer_accept(m_state.drag_offer, m_state.drag_serial, m_state.drag_mime.characters());
}

void WaylandDataDevice::on_drag_drop()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.drag_window >= 0 && m_client.m_input.drag_dropped)
        m_client.m_input.drag_dropped(m_state.drag_window, adjusted_drag_position(), m_client.m_input_state.pointer_buttons, m_client.current_modifiers(), m_state.drag_received);
    if (m_state.drag_offer) {
        wl_data_offer_finish(m_state.drag_offer);
        forget_offer(m_state.drag_offer);
        wl_data_offer_destroy(m_state.drag_offer);
        m_state.drag_offer = nullptr;
    }
    m_state.drag_window = -1;
    m_state.drag_received.clear();
}

void WaylandDataDevice::on_drag_leave()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_state.drag_offer) {
        forget_offer(m_state.drag_offer);
        wl_data_offer_destroy(m_state.drag_offer);
        m_state.drag_offer = nullptr;
    }
    m_state.drag_window = -1;
    m_state.drag_received.clear();
}

// An offer we are about to destroy must not be reachable from the selection state
// any more (the drag and the selection share pending_offers / offer_mime_types).
void WaylandDataDevice::forget_offer(wl_data_offer* offer)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.offer_mime_types.remove(reinterpret_cast<u64>(offer));
    if (m_state.current_offer == offer)
        m_state.current_offer = nullptr;
    for (size_t i = m_state.pending_offers.size(); i > 0; --i) {
        if (m_state.pending_offers[i - 1] == offer)
            m_state.pending_offers.remove(i - 1);
    }
}

// --- clipboard ---------------------------------------------------------------

Vector<ByteString> WaylandDataDevice::clipboard_mime_types() const
{
    MutexLocker locker(m_client.m_mutex);
    // If we own the selection, the offers are the canonical list: the data-source
    // round trip would block the same event loop that has to answer it.
    if (m_state.selection_source && !m_state.clipboard_offers.is_empty()) {
        Vector<ByteString> mimes;
        mimes.ensure_capacity(m_state.clipboard_offers.size());
        for (auto const& it : m_state.clipboard_offers)
            mimes.append(ByteString { it.key.view() });
        return mimes;
    }

    if (!m_state.current_offer)
        return {};

    if (auto it = m_state.offer_mime_types.find(reinterpret_cast<u64>(m_state.current_offer)); it != m_state.offer_mime_types.end()) {
        Vector<ByteString> mimes;
        mimes.ensure_capacity(it->value.size());
        for (auto const& mime : it->value)
            mimes.append(ByteString { mime.view() });
        return mimes;
    }
    return {};
}

ErrorOr<ByteBuffer> WaylandDataDevice::read_clipboard_for_mime(ByteString const& wanted_mime, ByteString& out_mime_type)
{
    out_mime_type = {};
    if (wanted_mime.is_empty())
        return Error::from_string_literal("LibWM: no MIME type requested");

    int fds[2] = { -1, -1 };
    {
        MutexLocker locker(m_client.m_mutex);
        // If we are the clipboard owner, serve the stored offers directly.
        if (m_state.selection_source && !m_state.clipboard_offers.is_empty()) {
            if (auto it = m_state.clipboard_offers.find(wanted_mime); it != m_state.clipboard_offers.end()) {
                out_mime_type = it->key;
                return ByteBuffer::copy(it->value);
            }

            // text/plain is offered under a few aliases; accept any of them.
            if (wanted_mime == "text/plain"sv || wanted_mime.starts_with("text/plain;"sv)) {
                for (auto const& alias : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "STRING"sv }) {
                    if (auto it = m_state.clipboard_offers.find(ByteString(alias)); it != m_state.clipboard_offers.end()) {
                        out_mime_type = ByteString("text/plain");
                        return ByteBuffer::copy(it->value);
                    }
                }
            }

            return Error::from_string_literal("LibWM: selection does not offer the requested MIME type");
        }

        if (!m_state.current_offer || !m_client.m_display)
            return Error::from_string_literal("LibWM: no clipboard selection");

        if (pipe(fds) != 0)
            return Error::from_errno(errno);

        wl_data_offer_receive(m_state.current_offer, wanted_mime.characters(), fds[1]);
        wl_display_flush(m_client.m_display);
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

ErrorOr<ByteBuffer> WaylandDataDevice::read_clipboard(ByteString& out_mime_type)
{
    out_mime_type = {};

    ByteString mime;
    {
        MutexLocker locker(m_client.m_mutex);
        if (m_state.selection_source && !m_state.clipboard_offers.is_empty()) {
            for (auto const& preferred : { "text/plain"sv, "text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "text/uri-list"sv, "image/png"sv, "image/x-serenityos"sv }) {
                if (m_state.clipboard_offers.contains(ByteString(preferred))) {
                    mime = ByteString(preferred);
                    break;
                }
            }
            if (mime.is_empty())
                mime = m_state.clipboard_offers.begin()->key;
        } else {
            if (!m_state.current_offer)
                return Error::from_string_literal("LibWM: no clipboard selection");
            mime = preferred_mime_for(m_state.current_offer);
            if (mime.is_empty())
                return Error::from_string_literal("LibWM: selection offers no usable type");
        }
    }

    return read_clipboard_for_mime(mime, out_mime_type);
}

void WaylandDataDevice::write_clipboard(HashMap<ByteString, ByteBuffer> offers)
{
    MutexLocker locker(m_client.m_mutex);
    m_state.clipboard_offers = move(offers);

    if (!m_state.manager || !m_state.device) {
        dbgln("LibWM/Wayland: cannot set clipboard (no data device)");
        return;
    }

    if (m_state.selection_source) {
        wl_data_source_destroy(m_state.selection_source);
        m_state.selection_source = nullptr;
    }

    if (m_state.clipboard_offers.is_empty()) {
        wl_data_device_set_selection(m_state.device, nullptr, m_client.m_input_state.last_serial);
        wl_display_flush(m_client.m_display);
        return;
    }

    m_state.selection_source = wl_data_device_manager_create_data_source(m_state.manager);
    wl_data_source_add_listener(m_state.selection_source, &s_data_source_listener, this);
    for (auto const& it : m_state.clipboard_offers)
        wl_data_source_offer(m_state.selection_source, it.key.characters());
    wl_data_device_set_selection(m_state.device, m_state.selection_source, m_client.m_input_state.last_serial);
    wl_display_flush(m_client.m_display);
    dbgln("LibWM/Wayland: set clipboard ({} representation(s), serial {})", m_state.clipboard_offers.size(), m_client.m_input_state.last_serial);
}

}
