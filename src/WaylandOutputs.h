/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Size.h>

struct wl_output;
struct zxdg_output_manager_v1;

namespace LibWM {

class WaylandClient;

// wl_output / zxdg_output_v1: the screen's logical size and scale, and the scale
// the shim tells clients to render at.
//
// Two scales are tracked. The *output* scale is what the compositor reports
// (wl_output.scale, refined by the zxdg_output logical size or a surface's
// wp_fractional_scale preferred scale). The *client* scale is what LibGUI
// backing stores are rendered at: ceil(client_scale_v120 / 120), with the
// fractional remainder resampled at presentation. They differ when
// LIBWM_CLIENT_SCALE overrides the compositor.
//
// Scales are in 1/120ths ("v120"), as in wp_fractional_scale_v1. Shares
// WaylandClient's mutex.
class WaylandOutputs {
public:
    explicit WaylandOutputs(WaylandClient&);

    Gfx::IntSize screen_size() const;
    int output_scale() const;
    int output_scale_v120() const;
    int client_scale() const;
    int client_scale_v120() const;
    void set_client_scale(int scale);
    void set_client_scale_v120(int scale_v120);

    // A surface's wp_fractional_scale_v1 preferred scale (more precise than
    // anything wl_output reports).
    void set_preferred_scale_v120(int scale_v120);

    // --- registry and protocol events ---
    void add_output(wl_output*);
    void set_xdg_output_manager(zxdg_output_manager_v1*);
    void on_output_mode(Gfx::IntSize physical_size);
    void on_output_scale(int factor);
    void on_output_logical_size(Gfx::IntSize logical_size);
    void on_output_done();

private:
    void infer_scale_from_logical_size();
    void listen_to_xdg_output(wl_output*);

    WaylandClient& m_client;
    wl_output* m_primary { nullptr };
    Vector<wl_output*> m_outputs;
    zxdg_output_manager_v1* m_xdg_manager { nullptr };
    Gfx::IntSize m_physical_size;
    Gfx::IntSize m_screen_size;
    int m_scale { 1 };
    int m_scale_v120 { 120 };
    int m_client_scale { 1 };
    int m_client_scale_v120 { 120 };
    bool m_have_logical_size { false };
};

}
