/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// xdg-activation-v1: ask the compositor to raise and focus a window.
//
// A launcher can hand its activation token to the application through
// XDG_ACTIVATION_TOKEN (the standard mechanism); LibWM uses that for the first
// window it presents. Otherwise it requests a token from the compositor with the
// seat's last input serial, which covers a window opened from a click or a menu.
// SerenityDE's compositor already implements the server side and also focuses
// newly mapped windows; other compositors need one of these tokens to raise a
// launcher-spawned window.

#include "Environment.h"
#include "WaylandClient.h"
#include "WaylandEvents.h"
#include "WaylandSeat.h"
#include "xdg-activation-v1-client-protocol.h"
#include <stdlib.h>

namespace LibWM {

namespace {

struct ActivationTokenData {
    WaylandClient* client { nullptr };
    i32 window_id { -1 };
};

xdg_activation_token_v1_listener const s_activation_token_listener = {
    .done = WaylandEvents::activation_token_done,
};

}

void WaylandEvents::activation_token_done(void* data, xdg_activation_token_v1* token, char const* token_string)
{
    auto* token_data = static_cast<ActivationTokenData*>(data);
    token_data->client->on_activation_token_done(token_data->window_id, token, token_string);
    delete token_data;
}

void WaylandClient::activate_window(i32 window_id)
{
    MutexLocker locker(m_mutex);
    auto* window_surface = find(window_id);
    if (!m_activation || !window_surface || window_surface->is_layer || !window_surface->surface)
        return;
    if (window_surface->activation_requested)
        return;
    window_surface->activation_requested = true;

    if (auto const* env_token = getenv("XDG_ACTIVATION_TOKEN"); env_token && *env_token && !m_activation_token_used) {
        xdg_activation_v1_activate(m_activation, env_token, window_surface->surface);
        m_activation_token_used = true;
        trace("LibWM/Wayland: activated window {} with XDG_ACTIVATION_TOKEN", window_id);
        return;
    }

    auto const serial = m_seat->last_serial();
    auto* seat = m_seat->wayland_seat();
    if (!serial || !seat) {
        trace("LibWM/Wayland: no input serial for window {}; not requesting activation", window_id);
        return;
    }

    auto* token = xdg_activation_v1_get_activation_token(m_activation);
    if (!token)
        return;
    auto* data = new ActivationTokenData { this, window_id };
    xdg_activation_token_v1_add_listener(token, &s_activation_token_listener, data);
    xdg_activation_token_v1_set_serial(token, serial, seat);
    xdg_activation_token_v1_commit(token);
    trace("LibWM/Wayland: requested activation token for window {} (serial {})", window_id, serial);
}

void WaylandClient::on_activation_token_done(i32 window_id, xdg_activation_token_v1* token, char const* token_string)
{
    MutexLocker locker(m_mutex);
    if (m_activation && token_string && *token_string) {
        if (auto* window_surface = find(window_id); window_surface && window_surface->surface) {
            xdg_activation_v1_activate(m_activation, token_string, window_surface->surface);
            wl_display_flush(m_display);
            trace("LibWM/Wayland: activated window {}", window_id);
        }
    }
    if (token)
        xdg_activation_token_v1_destroy(token);
}

}
