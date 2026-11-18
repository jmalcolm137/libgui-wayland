/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>

struct wl_buffer;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_surface;

namespace LibWM {

class WaylandClient;

// wl_data_device: the clipboard (selection) and drag-and-drop, in both
// directions. Shares WaylandClient's mutex; drag events reach the application
// through WaylandClient's InputCallbacks.
class WaylandDataDevice {
public:
    explicit WaylandDataDevice(WaylandClient&);

    // --- clipboard ---
    // Called when the compositor's selection changes; the mime type is a
    // preferred Serenity-side type (e.g. "text/plain").
    void set_clipboard_changed_callback(Function<void(ByteString const&)>);
    // The MIME types the current selection offers, in the source's order.
    Vector<ByteString> clipboard_mime_types() const;
    // Fetch the current selection, preferring text then files/images. Blocks until the source closes the pipe.
    ErrorOr<ByteBuffer> read_clipboard(ByteString& out_mime_type);
    // Fetch one specific representation of the current selection. Blocks until the source closes the pipe.
    ErrorOr<ByteBuffer> read_clipboard_for_mime(ByteString const& mime_type, ByteString& out_mime_type);
    // Advertise a set of mime-type -> bytes representations as the selection.
    void write_clipboard(HashMap<ByteString, ByteBuffer> offers);
    // Whether this client currently owns the selection.
    bool owns_clipboard() const;

    // --- drag and drop ---
    // The source starts a drag from the window under the pointer; the target
    // receives enter/motion/drop with the mime data.
    bool start_drag(ByteString const& text, HashMap<String, ByteBuffer> const& mime_data, Gfx::ShareableBitmap const& bitmap);
    void set_accepts_drag(bool accepts);

    // --- registry and protocol events ---
    void bind(wl_data_device_manager*);
    // Creates the device once both the manager and a seat exist.
    void maybe_create_data_device();
    void on_data_offer(wl_data_offer*);
    void on_data_offer_mime(wl_data_offer*, char const* mime_type);
    void on_selection(wl_data_offer*);
    void on_source_send(wl_data_source*, char const* mime_type, int fd);
    void on_source_cancelled(wl_data_source*);
    void on_drag_enter(u32 serial, wl_data_offer*, wl_surface*, Gfx::IntPoint position);
    void on_drag_motion(Gfx::IntPoint position);
    void on_drag_drop();
    void on_drag_leave();
    void on_drag_source_cancelled(wl_data_source*);
    void on_drag_source_drop_performed();
    void on_drag_source_finished();
    void on_drag_source_action(u32 action);

private:
    ByteString preferred_mime_for(wl_data_offer*) const;
    ByteBuffer receive_offer_data(wl_data_offer*, ByteString const& mime);
    Gfx::IntPoint adjusted_drag_position() const;
    // Drop an offer we are about to destroy from the selection state (the drag
    // and the selection share the pending-offer list).
    void forget_offer(wl_data_offer*);

    struct State {
        wl_data_device_manager* manager { nullptr };
        wl_data_device* device { nullptr };
        wl_data_offer* current_offer { nullptr };
        Vector<wl_data_offer*> pending_offers;
        HashMap<u64, Vector<ByteString>> offer_mime_types;
        wl_data_source* selection_source { nullptr };
        HashMap<ByteString, ByteBuffer> clipboard_offers;
        Function<void(ByteString const&)> on_clipboard_changed;
        // Drag-and-drop state (see on_drag_*).
        HashMap<String, ByteBuffer> drag_payload;
        u32 drag_action { 0 };
        wl_data_source* drag_source { nullptr };
        wl_surface* drag_icon { nullptr };
        wl_buffer* drag_icon_buffer { nullptr };
        Optional<Core::AnonymousBuffer> drag_icon_anon;
        wl_data_offer* drag_offer { nullptr };
        i32 drag_window { -1 };
        Gfx::IntPoint drag_position;
        u32 drag_serial { 0 };
        bool accepts_drag { false };
        bool drag_dropped { false };
        ByteString drag_mime;
        HashMap<String, ByteBuffer> drag_received;
    };

    WaylandClient& m_client;
    State m_state;
};

}
