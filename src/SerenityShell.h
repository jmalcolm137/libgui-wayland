/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

struct serenity_toplevel;
struct serenity_window_manager;
struct wl_surface;

namespace LibWM {

class WaylandClient;

// The optional `serenity-window` protocol (src/protocol/serenity-window.xml),
// which a compositor offers to integrate with these applications as SerenityOS's
// own WindowServer would. Everything here degrades gracefully on a compositor
// without it: settings fall back to defaults, there is no wallpaper or capture,
// and windows simply get no compositor-driven chrome.
//
// It carries two kinds of state:
//  - desktop-owned configuration (host config, key/value host settings, the
//    wallpaper, screen captures), which the shim answers an application's
//    WindowServer config requests from instead of reading desktop files;
//  - per-window `serenity_toplevel` objects, through which the compositor drives
//    the menubar and reports where it placed a window.
//
// It shares WaylandClient's mutex.
class SerenityShell {
public:
    explicit SerenityShell(WaylandClient&);

    bool is_available() const;
    void bind(serenity_window_manager*);

    // --- desktop-owned configuration ---
    struct HostConfig {
        u32 workspace_rows { 1 };
        u32 workspace_columns { 1 };
        double accel_min { 0.5 };
        double accel_max { 3.5 };
        u32 scroll_step_min { 1 };
        int double_click_min { 100 };
        int double_click_max { 900 };
    };
    HostConfig host_config() const;

    // Settings keyed semantically (see the protocol). The compositor sends them;
    // changes are forwarded back, so the shim never touches the desktop's files.
    ByteString host_setting(StringView key, StringView fallback = {}) const;
    void set_host_setting(StringView key, StringView value);

    // Hands a wallpaper the application chose to the desktop, which stores and
    // shows it; clients read it back from the `wallpaper.file` host setting.
    void set_host_wallpaper(Gfx::Bitmap const*);

    // The Magnifier asks for a region of the screen; the compositor pushes it
    // back asynchronously, and captured_bitmap() returns the most recent one so
    // a capture never blocks the caller.
    RefPtr<Gfx::Bitmap> captured_bitmap() const;
    void request_capture(Gfx::IntRect const&);
    // Capture `region` and wait for it (up to `timeout_ms`), dispatching Wayland
    // on the calling thread. Null without the protocol or on timeout.
    RefPtr<Gfx::Bitmap> capture_now(Gfx::IntRect const&, int timeout_ms);

    // Runs after the compositor delivers a new set of host settings. Owned like
    // the display-link callback: installed only if unset, cleared only by `owner`.
    void set_host_settings_changed_callback(void* owner, Function<void()>);
    void clear_host_settings_changed_callback(void* owner);

    void set_dismiss_menus_callback(Function<void()>);
    void set_menubar_visibility_callback(Function<void(i32 window_id, bool visible)>);
    void set_toplevel_position_callback(Function<void(i32 window_id, Gfx::IntPoint position)>);

    // --- per-window chrome ---
    // Returns null without the protocol. Window mode and launch origin apply to
    // xdg toplevels; layer surfaces pass nothing and only get position reports.
    serenity_toplevel* create_toplevel(wl_surface*, Optional<i32> window_mode = {}, Gfx::IntRect const& launch_origin = {});
    void destroy_toplevel(serenity_toplevel*);
    static bool supports_menubar_inset(serenity_toplevel const*);
    void set_menubar_inset(serenity_toplevel*, int inset);
    void set_always_on_top(serenity_toplevel*, bool);
    void set_icon(serenity_toplevel*, Gfx::Bitmap const*);

    // --- protocol events (called from the listeners in SerenityShell.cpp) ---
    void on_host_config(u32 workspace_rows, u32 workspace_columns, double accel_min, double accel_max, u32 scroll_step_min, int double_click_min, int double_click_max);
    void on_host_settings(StringView blob);
    void on_captured(u32 serial, i32 width, i32 height, i32 pitch, u32 format, i32 fd);
    void on_dismiss_menus();
    void on_menubar_visibility(serenity_toplevel*, bool visible);
    void on_toplevel_position(serenity_toplevel*, i32 x, i32 y);

private:
    WaylandClient& m_client;
    serenity_window_manager* m_manager { nullptr };

    HostConfig m_host_config;
    HashMap<ByteString, ByteString> m_host_settings;
    RefPtr<Gfx::Bitmap> m_captured;
    u32 m_capture_serial { 0 };
    u32 m_captured_serial { 0 };

    Function<void()> m_host_settings_changed;
    void* m_host_settings_changed_owner { nullptr };
    Function<void()> m_dismiss_menus;
    Function<void(i32, bool)> m_menubar_visibility_changed;
    Function<void(i32, Gfx::IntPoint)> m_toplevel_position_changed;
};

}
