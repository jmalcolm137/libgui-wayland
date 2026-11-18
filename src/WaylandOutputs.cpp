/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandOutputs.h"
#include "WaylandClient.h"
#include <wayland-client.h>

#include "xdg-output-client-protocol.h"

namespace LibWM {

using MutexLocker = WaylandClient::MutexLocker;

// --- wl_output ----------------------------------------------------------------

static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, char const*, char const*, int32_t) { }

static void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t)
{
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    static_cast<WaylandOutputs*>(data)->on_output_mode({ width, height });
}

static void output_done(void* data, wl_output*)
{
    static_cast<WaylandOutputs*>(data)->on_output_done();
}

static void output_scale(void* data, wl_output*, int32_t factor)
{
    static_cast<WaylandOutputs*>(data)->on_output_scale(factor);
}

static void output_name(void*, wl_output*, char const*) { }
static void output_description(void*, wl_output*, char const*) { }

static wl_output_listener const s_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

// --- zxdg_output_v1 (logical size under fractional scaling) -------------------

static void xdg_output_logical_position(void*, zxdg_output_v1*, int32_t, int32_t) { }

static void xdg_output_logical_size(void* data, zxdg_output_v1*, int32_t width, int32_t height)
{
    static_cast<WaylandOutputs*>(data)->on_output_logical_size({ width, height });
}

static void xdg_output_done(void*, zxdg_output_v1*) { }
static void xdg_output_name(void*, zxdg_output_v1*, char const*) { }
static void xdg_output_description(void*, zxdg_output_v1*, char const*) { }

static zxdg_output_v1_listener const s_xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_logical_size,
    .done = xdg_output_done,
    .name = xdg_output_name,
    .description = xdg_output_description,
};

WaylandOutputs::WaylandOutputs(WaylandClient& client)
    : m_client(client)
{
}

Gfx::IntSize WaylandOutputs::screen_size() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_screen_size;
}

int WaylandOutputs::output_scale() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_scale;
}

int WaylandOutputs::output_scale_v120() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_scale_v120;
}

int WaylandOutputs::client_scale() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_client_scale;
}

int WaylandOutputs::client_scale_v120() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_client_scale_v120;
}

void WaylandOutputs::set_client_scale(int scale)
{
    MutexLocker locker(m_client.m_mutex);
    int const clamped = max(1, scale);
    m_client_scale = clamped;
    m_client_scale_v120 = clamped * 120;
}

void WaylandOutputs::set_client_scale_v120(int scale_v120)
{
    MutexLocker locker(m_client.m_mutex);
    int const clamped = scale_v120 < 60 ? 120 : scale_v120;
    m_client_scale_v120 = clamped;
    m_client_scale = max(1, (clamped + 119) / 120);
}

void WaylandOutputs::set_preferred_scale_v120(int scale_v120)
{
    MutexLocker locker(m_client.m_mutex);
    m_scale_v120 = scale_v120;
    m_scale = max(1, (scale_v120 + 119) / 120);
}

void WaylandOutputs::listen_to_xdg_output(wl_output* output)
{
    auto* xdg_output = zxdg_output_manager_v1_get_xdg_output(m_xdg_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &s_xdg_output_listener, this);
}

void WaylandOutputs::add_output(wl_output* output)
{
    MutexLocker locker(m_client.m_mutex);
    m_primary = output;
    m_outputs.append(output);
    wl_output_add_listener(output, &s_output_listener, this);
    if (m_xdg_manager)
        listen_to_xdg_output(output);
}

void WaylandOutputs::set_xdg_output_manager(zxdg_output_manager_v1* manager)
{
    MutexLocker locker(m_client.m_mutex);
    m_xdg_manager = manager;
    for (auto* output : m_outputs)
        listen_to_xdg_output(output);
}

void WaylandOutputs::on_output_mode(Gfx::IntSize physical_size)
{
    MutexLocker locker(m_client.m_mutex);
    m_physical_size = physical_size;
}

void WaylandOutputs::on_output_scale(int factor)
{
    MutexLocker locker(m_client.m_mutex);
    m_scale = factor > 0 ? factor : 1;
    if (!m_have_logical_size)
        m_scale_v120 = m_scale * 120;
}

// Under fractional scaling wl_output only reports an integer scale; the real
// factor is physical width / logical width. Snap to the nearest 1/8 step (15/120)
// when within rounding error, since compositors offer those.
void WaylandOutputs::infer_scale_from_logical_size()
{
    if (m_physical_size.is_empty() || m_screen_size.width() <= 0)
        return;
    int raw_v120 = static_cast<int>(__builtin_lround(static_cast<double>(m_physical_size.width()) * 120.0 / static_cast<double>(m_screen_size.width())));
    int const snapped_v120 = ((raw_v120 + 7) / 15) * 15;
    if (__builtin_abs(raw_v120 - snapped_v120) <= 2)
        raw_v120 = snapped_v120;
    m_scale_v120 = max(120, raw_v120);
    m_scale = max(1, (m_scale_v120 + 119) / 120);
}

void WaylandOutputs::on_output_logical_size(Gfx::IntSize logical_size)
{
    MutexLocker locker(m_client.m_mutex);
    if (logical_size.is_empty())
        return;
    m_have_logical_size = true;
    m_screen_size = logical_size;
    infer_scale_from_logical_size();
}

void WaylandOutputs::on_output_done()
{
    MutexLocker locker(m_client.m_mutex);
    // zxdg_output_v1 gives the real logical size under fractional scaling and
    // takes precedence over mode/scale.
    if (m_have_logical_size) {
        infer_scale_from_logical_size();
        return;
    }
    if (m_physical_size.is_empty())
        return;
    m_screen_size = { m_physical_size.width() / m_scale, m_physical_size.height() / m_scale };
    m_scale_v120 = m_scale * 120;
}

}
