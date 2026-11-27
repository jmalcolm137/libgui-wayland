/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Screen capture through the standard org.freedesktop.portal.Screenshot portal.
//
// A desktop that does not speak serenity-window can still provide screenshots:
// the screenshot portal is the portable route (xdg-desktop-portal plus a
// backend such as -wlr, -gtk or the desktop's own). LibWM asks for a
// non-interactive screenshot, reads the returned PNG and hands it to the
// caller. The serenity-window capture is tried first when the compositor offers
// it, so SerenityDE keeps its own (faster, direct) path.

#include "ScreenshotPortal.h"
#include "Environment.h"
#include <string.h>
#include <unistd.h>

#ifdef LIBWM_HAVE_SDBUS
#    include <LibGfx/Bitmap.h>
#    include <LibURL/URL.h>
#    include <systemd/sd-bus.h>
#    include <time.h>
#endif

namespace LibWM {

#ifdef LIBWM_HAVE_SDBUS

namespace {

constexpr char const* portal_service = "org.freedesktop.portal.Desktop";
constexpr char const* portal_path = "/org/freedesktop/portal/desktop";
constexpr char const* screenshot_interface = "org.freedesktop.portal.Screenshot";

struct ResponseState {
    ByteString uri;
    u32 response { 0xffffffff };
    bool done { false };
};

int on_response(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* state = static_cast<ResponseState*>(userdata);
    u32 response = 0;
    if (sd_bus_message_read(message, "u", &response) < 0)
        return 0;
    state->response = response;

    if (sd_bus_message_enter_container(message, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
            char const* key = nullptr;
            if (sd_bus_message_read(message, "s", &key) >= 0 && key && !strcmp(key, "uri")) {
                char const* uri = nullptr;
                if (sd_bus_message_read(message, "v", "s", &uri) > 0 && uri)
                    state->uri = uri;
            } else {
                sd_bus_message_skip(message, "v");
            }
            sd_bus_message_exit_container(message);
        }
        sd_bus_message_exit_container(message);
    }
    state->done = true;
    return 1;
}

u64 now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000 + static_cast<u64>(ts.tv_nsec) / 1'000'000;
}

ByteString path_from_uri(ByteString const& uri)
{
    if (!uri.starts_with("file://"sv))
        return { };
    auto path = uri.substring_view(7);
    return URL::percent_decode(path);
}

} // namespace

ScreenshotPortal::ScreenshotPortal()
{
    if (sd_bus_open_user(&m_bus) < 0) {
        trace("LibWM: no session bus; the screenshot portal is unavailable");
        m_bus = nullptr;
        return;
    }
    sd_bus_set_method_call_timeout(m_bus, 2'000'000);
    trace("LibWM: session bus open for the screenshot portal");
}

ScreenshotPortal::~ScreenshotPortal()
{
    if (m_bus)
        sd_bus_unref(m_bus);
}

ScreenshotPortal& ScreenshotPortal::the()
{
    static ScreenshotPortal portal;
    return portal;
}

RefPtr<Gfx::Bitmap> ScreenshotPortal::capture(int timeout_ms)
{
    if (!m_bus)
        return nullptr;

    // Reuse a very recent frame: the Magnifier asks on every display-link tick,
    // and a portal round trip per tick would be wasteful.
    u64 const now = now_ms();
    if (m_last_capture && m_last_capture_ms != 0 && now - m_last_capture_ms < 100)
        return m_last_capture;

    // A handle token is an object-path element: letters, digits and underscores.
    auto token = ByteString::formatted("libwm_{}_{}", getpid(), ++m_request_count);
    sd_bus_message* message = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    if (sd_bus_message_new_method_call(m_bus, &message, portal_service,
            portal_path, screenshot_interface,
            "Screenshot")
        < 0)
        return nullptr;
    sd_bus_message_append(message, "s", "");
    sd_bus_message_open_container(message, 'a', "{sv}");
    sd_bus_message_open_container(message, 'e', "sv");
    sd_bus_message_append(message, "s", "handle_token");
    sd_bus_message_open_container(message, 'v', "s");
    sd_bus_message_append(message, "s", token.characters());
    sd_bus_message_close_container(message);
    sd_bus_message_close_container(message);
    sd_bus_message_open_container(message, 'e', "sv");
    sd_bus_message_append(message, "s", "interactive");
    sd_bus_message_open_container(message, 'v', "b");
    sd_bus_message_append(message, "b", 0);
    sd_bus_message_close_container(message);
    sd_bus_message_close_container(message);
    sd_bus_message_close_container(message);

    int const call_result = sd_bus_call(m_bus, message, timeout_ms * 1000, &error, &reply);
    sd_bus_message_unref(message);
    if (call_result < 0) {
        if (!m_reported_failure) {
            trace("LibWM: screenshot portal unavailable: {}",
                error.message ? error.message : "no portal");
            m_reported_failure = true;
        }
        sd_bus_error_free(&error);
        return nullptr;
    }

    char const* handle = nullptr;
    if (sd_bus_message_read(reply, "o", &handle) < 0 || !handle) {
        sd_bus_message_unref(reply);
        return nullptr;
    }

    auto match = ByteString::formatted("type='signal',interface='org.freedesktop.portal."
                                       "Request',member='Response',path='{}'",
        handle);
    ResponseState state;
    sd_bus_slot* slot = nullptr;
    if (sd_bus_add_match(m_bus, &slot, match.characters(), on_response, &state) < 0) {
        sd_bus_message_unref(reply);
        return nullptr;
    }
    sd_bus_message_unref(reply);

    u64 const deadline = now_ms() + static_cast<u64>(timeout_ms);
    while (!state.done && now_ms() < deadline) {
        int const processed = sd_bus_process(m_bus, nullptr);
        if (processed < 0)
            break;
        if (processed > 0)
            continue;
        u64 const remaining = deadline > now_ms() ? deadline - now_ms() : 0;
        if (remaining == 0)
            break;
        (void)sd_bus_wait(m_bus, remaining * 1000);
    }
    sd_bus_slot_unref(slot);

    if (!state.done || state.response != 0 || state.uri.is_empty()) {
        trace("LibWM: screenshot portal returned no image (response {})",
            state.response);
        return nullptr;
    }

    auto path = path_from_uri(state.uri);
    if (path.is_empty()) {
        trace("LibWM: screenshot portal returned a non-file URI");
        return nullptr;
    }
    auto loaded = Gfx::Bitmap::load_from_file(path);
    ::unlink(path.characters());
    if (loaded.is_error()) {
        trace("LibWM: cannot load the portal screenshot '{}'", path);
        return nullptr;
    }
    m_last_capture = loaded.release_value();
    m_last_capture_ms = now_ms();
    trace("LibWM: screenshot portal returned {}x{}", m_last_capture->width(),
        m_last_capture->height());
    return m_last_capture;
}

#else

ScreenshotPortal::ScreenshotPortal() = default;
ScreenshotPortal::~ScreenshotPortal() = default;

ScreenshotPortal& ScreenshotPortal::the()
{
    static ScreenshotPortal portal;
    return portal;
}

bool ScreenshotPortal::available() const { return false; }

RefPtr<Gfx::Bitmap> ScreenshotPortal::capture(int) { return nullptr; }

#endif

} // namespace LibWM
