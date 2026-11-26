/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SerenityShell.h"
#include "Environment.h"
#include "WaylandClient.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "serenity-window-client-protocol.h"

namespace LibWM {

using MutexLocker = WaylandClient::MutexLocker;

// --- serenity_window_manager --------------------------------------------------

static void manager_host_config(void* data, serenity_window_manager*,
    uint32_t workspace_rows, uint32_t workspace_columns, wl_fixed_t accel_min, wl_fixed_t accel_max,
    uint32_t scroll_step_min, int32_t double_click_min, int32_t double_click_max)
{
    static_cast<SerenityShell*>(data)->on_host_config(workspace_rows, workspace_columns,
        wl_fixed_to_double(accel_min), wl_fixed_to_double(accel_max),
        scroll_step_min, double_click_min, double_click_max);
}

static void manager_host_settings(void* data, serenity_window_manager*, char const* settings)
{
    static_cast<SerenityShell*>(data)->on_host_settings(StringView { settings, strlen(settings) });
}

static void manager_dismiss_menus(void* data, serenity_window_manager*)
{
    static_cast<SerenityShell*>(data)->on_dismiss_menus();
}

static void manager_captured(void* data, serenity_window_manager*, uint32_t serial, int32_t width, int32_t height, int32_t pitch, uint32_t format, int32_t fd)
{
    static_cast<SerenityShell*>(data)->on_captured(serial, width, height, pitch, format, fd);
}

static serenity_window_manager_listener const s_manager_listener = {
    .host_settings = manager_host_settings,
    .dismiss_menus = manager_dismiss_menus,
    .host_config = manager_host_config,
    .captured = manager_captured,
};

// --- serenity_toplevel --------------------------------------------------------

static void toplevel_menubar_visibility(void* data, serenity_toplevel* resource, int32_t visible)
{
    static_cast<SerenityShell*>(data)->on_menubar_visibility(resource, visible != 0);
}

static void toplevel_position(void* data, serenity_toplevel* resource, int32_t x, int32_t y)
{
    static_cast<SerenityShell*>(data)->on_toplevel_position(resource, x, y);
}

static serenity_toplevel_listener const s_toplevel_listener = {
    .menubar_visibility = toplevel_menubar_visibility,
    .position = toplevel_position,
};

// Copy a compositor-provided shm image (wallpaper, capture) into a Bitmap and
// close the fd.
static RefPtr<Gfx::Bitmap> bitmap_from_fd(i32 width, i32 height, i32 pitch, u32 format, i32 fd)
{
    RefPtr<Gfx::Bitmap> result;
    if (fd >= 0 && width > 0 && height > 0 && pitch > 0) {
        auto const size = static_cast<size_t>(pitch) * static_cast<size_t>(height);
        auto* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
            if (auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(format), { width, height }); !bitmap.is_error()) {
                auto& created = *bitmap.value();
                auto const row_bytes = min(static_cast<size_t>(pitch), static_cast<size_t>(created.pitch()));
                for (int y = 0; y < height; ++y)
                    memcpy(created.scanline(y), static_cast<u8 const*>(mapped) + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);
                result = bitmap.release_value();
            }
            munmap(mapped, size);
        }
    }
    if (fd >= 0)
        close(fd);
    return result;
}

// A sealed-size memfd holding `bitmap`'s pixels, or -1.
static int memfd_for_bitmap(char const* name, Gfx::Bitmap const& bitmap)
{
    size_t const size = static_cast<size_t>(bitmap.pitch()) * static_cast<size_t>(bitmap.height());
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return -1;
    }
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return -1;
    }
    for (int y = 0; y < bitmap.height(); ++y)
        memcpy(static_cast<u8*>(mapped) + static_cast<size_t>(y) * bitmap.pitch(), bitmap.scanline(y), static_cast<size_t>(bitmap.pitch()));
    munmap(mapped, size);
    return fd;
}

SerenityShell::SerenityShell(WaylandClient& client)
    : m_client(client)
{
}

bool SerenityShell::is_available() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_manager != nullptr;
}

void SerenityShell::bind(serenity_window_manager* manager)
{
    MutexLocker locker(m_client.m_mutex);
    m_manager = manager;
    if (manager)
        serenity_window_manager_add_listener(manager, &s_manager_listener, this);
}

// --- desktop-owned configuration ----------------------------------------------

SerenityShell::HostConfig SerenityShell::host_config() const
{
    MutexLocker locker(m_client.m_mutex);
    return m_host_config;
}

void SerenityShell::on_host_config(u32 workspace_rows, u32 workspace_columns, double accel_min, double accel_max, u32 scroll_step_min, int double_click_min, int double_click_max)
{
    MutexLocker locker(m_client.m_mutex);
    m_host_config.workspace_rows = workspace_rows > 0 ? workspace_rows : 1;
    m_host_config.workspace_columns = workspace_columns > 0 ? workspace_columns : 1;
    m_host_config.accel_min = accel_min;
    m_host_config.accel_max = accel_max;
    m_host_config.scroll_step_min = scroll_step_min;
    m_host_config.double_click_min = double_click_min;
    m_host_config.double_click_max = double_click_max;
    trace("LibWM: host config: workspaces {}x{}, accel {}-{}, scroll min {}, double-click {}-{}",
        m_host_config.workspace_rows, m_host_config.workspace_columns, m_host_config.accel_min, m_host_config.accel_max,
        m_host_config.scroll_step_min, m_host_config.double_click_min, m_host_config.double_click_max);
}

void SerenityShell::on_host_settings(StringView blob)
{
    MutexLocker locker(m_client.m_mutex);
    m_host_settings.clear();
    for (auto line : blob.split_view('\n')) {
        if (line.is_empty())
            continue;
        auto separator = line.find('=');
        if (!separator.has_value())
            continue;
        m_host_settings.set(line.substring_view(0, *separator).to_byte_string(), line.substring_view(*separator + 1).to_byte_string());
    }
    trace("LibWM: host settings: {} entries", m_host_settings.size());
    if (m_host_settings_changed)
        m_host_settings_changed();
}

void SerenityShell::set_host_settings_changed_callback(void* owner, Function<void()> callback)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_host_settings_changed_owner)
        return;
    m_host_settings_changed_owner = owner;
    m_host_settings_changed = move(callback);
}

void SerenityShell::clear_host_settings_changed_callback(void* owner)
{
    MutexLocker locker(m_client.m_mutex);
    if (m_host_settings_changed_owner != owner)
        return;
    m_host_settings_changed_owner = nullptr;
    m_host_settings_changed = nullptr;
}

ByteString SerenityShell::host_setting(StringView key, StringView fallback) const
{
    MutexLocker locker(m_client.m_mutex);
    // A deep copy: ByteString's refcount is not atomic, and the caller may be on
    // another thread.
    if (auto it = m_host_settings.find(ByteString { key }); it != m_host_settings.end())
        return ByteString { it->value.view() };
    return ByteString { fallback };
}

void SerenityShell::set_host_setting(StringView key, StringView value)
{
    MutexLocker locker(m_client.m_mutex);
    // Keep our own answers current, and hand the change to the desktop, which
    // applies and persists it.
    m_host_settings.set(ByteString { key }, ByteString { value });
    if (m_manager) {
        auto k = key.to_byte_string();
        auto v = value.to_byte_string();
        serenity_window_manager_set_host_setting(m_manager, k.characters(), v.characters());
        wl_display_flush(m_client.m_display);
    }
}

void SerenityShell::set_host_wallpaper(Gfx::Bitmap const* bitmap)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_manager)
        return;
    int fd = bitmap ? memfd_for_bitmap("libwm-wallpaper", *bitmap) : -1;
    // A `h` argument must be a real file descriptor: libwayland cannot dup a
    // -1, and aborts the whole request (breaking this connection with it).
    // "No wallpaper" is expressed by a zero width/height, so hand over a
    // valid placeholder fd in that case.
    bool const have_pixels = fd >= 0;
    if (!have_pixels)
        fd = memfd_create("libwm-wallpaper-none", MFD_CLOEXEC);
    serenity_window_manager_set_wallpaper(m_manager,
        have_pixels ? bitmap->width() : 0, have_pixels ? bitmap->height() : 0,
        have_pixels ? bitmap->pitch() : 0,
        have_pixels ? static_cast<uint32_t>(bitmap->format()) : 0, fd);
    wl_display_flush(m_client.m_display);
    if (fd >= 0)
        close(fd);
}

RefPtr<Gfx::Bitmap> SerenityShell::captured_bitmap() const
{
    MutexLocker locker(m_client.m_mutex);
    return copy_for_other_thread(m_captured.ptr());
}

void SerenityShell::request_capture(Gfx::IntRect const& region)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_manager || region.is_empty())
        return;
    serenity_window_manager_request_capture(m_manager, ++m_capture_serial,
        region.x(), region.y(), region.width(), region.height());
    wl_display_flush(m_client.m_display);
}

void SerenityShell::on_captured(u32 serial, i32 width, i32 height, i32 pitch, u32 format, i32 fd)
{
    MutexLocker locker(m_client.m_mutex);
    if (auto bitmap = bitmap_from_fd(width, height, pitch, format, fd))
        m_captured = move(bitmap);
    m_captured_serial = max(m_captured_serial, serial);
}

RefPtr<Gfx::Bitmap> SerenityShell::capture_now(Gfx::IntRect const& region, int timeout_ms)
{
    u32 serial = 0;
    {
        MutexLocker locker(m_client.m_mutex);
        if (!m_manager || region.is_empty())
            return nullptr;
        serial = m_capture_serial + 1;
    }
    request_capture(region);
    bool const arrived = m_client.dispatch_until([&] { return m_captured_serial >= serial; }, timeout_ms);
    MutexLocker locker(m_client.m_mutex);
    return arrived ? copy_for_other_thread(m_captured.ptr()) : nullptr;
}

void SerenityShell::set_dismiss_menus_callback(Function<void()> callback)
{
    MutexLocker locker(m_client.m_mutex);
    m_dismiss_menus = move(callback);
}

void SerenityShell::set_menubar_visibility_callback(Function<void(i32, bool)> callback)
{
    MutexLocker locker(m_client.m_mutex);
    m_menubar_visibility_changed = move(callback);
}

void SerenityShell::set_toplevel_position_callback(Function<void(i32, Gfx::IntPoint)> callback)
{
    MutexLocker locker(m_client.m_mutex);
    m_toplevel_position_changed = move(callback);
}

void SerenityShell::on_dismiss_menus()
{
    MutexLocker locker(m_client.m_mutex);
    if (m_dismiss_menus)
        m_dismiss_menus();
}

// --- per-window chrome ----------------------------------------------------------

serenity_toplevel* SerenityShell::create_toplevel(wl_surface* surface, Optional<i32> window_mode, Gfx::IntRect const& launch_origin)
{
    MutexLocker locker(m_client.m_mutex);
    if (!m_manager || !surface)
        return nullptr;
    auto* toplevel = serenity_window_manager_get_toplevel(m_manager, surface);
    if (!toplevel)
        return nullptr;
    serenity_toplevel_add_listener(toplevel, &s_toplevel_listener, this);
    if (window_mode.has_value())
        serenity_toplevel_set_window_mode(toplevel, static_cast<uint32_t>(*window_mode));
    if (!launch_origin.is_empty())
        serenity_toplevel_set_launch_origin(toplevel, launch_origin.x(), launch_origin.y(), launch_origin.width(), launch_origin.height());
    return toplevel;
}

void SerenityShell::destroy_toplevel(serenity_toplevel* toplevel)
{
    if (toplevel)
        serenity_toplevel_destroy(toplevel);
}

bool SerenityShell::supports_menubar_inset(serenity_toplevel const* toplevel)
{
    return toplevel && wl_proxy_get_version(reinterpret_cast<wl_proxy*>(const_cast<serenity_toplevel*>(toplevel))) >= 2;
}

void SerenityShell::set_menubar_inset(serenity_toplevel* toplevel, int inset)
{
    MutexLocker locker(m_client.m_mutex);
    if (!supports_menubar_inset(toplevel))
        return;
    serenity_toplevel_set_menubar_inset(toplevel, inset);
    wl_display_flush(m_client.m_display);
}

void SerenityShell::set_always_on_top(serenity_toplevel* toplevel, bool always_on_top)
{
    MutexLocker locker(m_client.m_mutex);
    if (!toplevel || wl_proxy_get_version(reinterpret_cast<wl_proxy*>(toplevel)) < 3)
        return;
    serenity_toplevel_set_always_on_top(toplevel, always_on_top ? 1 : 0);
    wl_display_flush(m_client.m_display);
}

void SerenityShell::set_icon(serenity_toplevel* toplevel, Gfx::Bitmap const* bitmap)
{
    MutexLocker locker(m_client.m_mutex);
    if (!toplevel)
        return;
    if (!bitmap || bitmap->width() <= 0 || bitmap->height() <= 0 || bitmap->pitch() <= 0) {
        serenity_toplevel_set_icon(toplevel, 0, 0, 0, 0, -1);
        wl_display_flush(m_client.m_display);
        return;
    }
    int fd = memfd_for_bitmap("libwm-window-icon", *bitmap);
    if (fd < 0)
        return;
    serenity_toplevel_set_icon(toplevel, bitmap->width(), bitmap->height(), bitmap->pitch(), 0, fd);
    close(fd);
    wl_display_flush(m_client.m_display);
}

void SerenityShell::on_menubar_visibility(serenity_toplevel* resource, bool visible)
{
    MutexLocker locker(m_client.m_mutex);
    for (auto const& it : m_client.m_windows) {
        if (it.value->chrome != resource)
            continue;
        it.value->menubar_visible = visible;
        if (m_menubar_visibility_changed)
            m_menubar_visibility_changed(it.value->window_id, visible);
        return;
    }
}

void SerenityShell::on_toplevel_position(serenity_toplevel* resource, i32 x, i32 y)
{
    MutexLocker locker(m_client.m_mutex);
    for (auto const& it : m_client.m_windows) {
        if (it.value->chrome != resource)
            continue;
        WaylandClient::MutexLocker surface_locker(it.value->mutex);
        // Layer surfaces (applets) are laid out by the compositor, so their
        // origin comes from here, not from their anchored corner.
        if (it.value->is_layer)
            it.value->layer_output_position = { x, y };
        else
            it.value->global_position = { x, y };
        if (m_toplevel_position_changed)
            m_toplevel_position_changed(it.value->window_id, Gfx::IntPoint { x, y });
        return;
    }
}

}
