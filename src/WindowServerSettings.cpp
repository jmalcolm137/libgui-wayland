/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowServerConnection.h"
#include "Environment.h"
#include "SerenityShell.h"
#include "WaylandClient.h"
#include "WaylandOutputs.h"
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/SystemTheme.h>
#include <WindowServer/SystemEffects.h>
#include <stdlib.h>
#include <string.h>

namespace {

// The wallpaper state is stored as raw pixels, not a PNG: encoding a full-screen
// image to PNG takes several hundred milliseconds and would stall the UI thread
// of whichever application applies it. Must match WallpaperHeader in the
// compositor (WindowServer/src/main.cpp).
struct WallpaperHeader {
    u32 magic;
    u32 format;
    u32 width;
    u32 height;
    u32 pitch;
};
constexpr u32 wallpaper_magic = 0x53445750; // "SDWP"

// Must match Config::state_directory() in the compositor. The wallpaper is
// persisted there as raw pixels so a client can load it on demand instead of
// every client receiving (and copying) its own wallpaper bitmap over Wayland.
ByteString wallpaper_state_directory()
{
    if (auto const* dir = getenv("SDE_STATE_DIR"); dir && *dir)
        return ByteString(dir);
    if (auto const* config = getenv("XDG_CONFIG_HOME"); config && *config)
        return ByteString::formatted("{}/sde", config);
    if (auto const* home = getenv("HOME"); home && *home)
        return ByteString::formatted("{}/.config/sde", home);
    return ByteString { "/tmp/sde" };
}

RefPtr<Gfx::Bitmap> read_wallpaper_state()
{
    auto path = ByteString::formatted("{}/wallpaper.raw", wallpaper_state_directory());
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return nullptr;
    auto raw = file.value()->read_until_eof();
    if (raw.is_error() || raw.value().size() <= sizeof(WallpaperHeader))
        return nullptr;

    WallpaperHeader header {};
    memcpy(&header, raw.value().data(), sizeof(header));
    if (header.magic != wallpaper_magic || header.width == 0 || header.height == 0 || header.pitch == 0)
        return nullptr;
    size_t const pixels = static_cast<size_t>(header.pitch) * static_cast<size_t>(header.height);
    if (raw.value().size() < sizeof(header) + pixels)
        return nullptr;

    auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(header.format), { static_cast<int>(header.width), static_cast<int>(header.height) });
    if (bitmap.is_error())
        return nullptr;
    auto& created = *bitmap.value();
    auto const row_bytes = min(static_cast<size_t>(header.pitch), static_cast<size_t>(created.pitch()));
    for (int y = 0; y < created.height(); ++y)
        memcpy(created.scanline(y), raw.value().data() + sizeof(header) + static_cast<size_t>(y) * header.pitch, row_bytes);
    return bitmap.release_value();
}

ByteString theme_broker_path()
{
    return LibWM::Environment::runtime_path("sde-theme"sv);
}

ByteString fonts_broker_path()
{
    return LibWM::Environment::runtime_path("sde-fonts"sv);
}

void write_fonts_broker(ByteString const& default_query, ByteString const& fixed_width_query, ByteString const& window_title_query)
{
    if (auto file = Core::File::open(fonts_broker_path(), Core::File::OpenMode::Write); !file.is_error())
        (void)file.value()->write_until_depleted(ByteString::formatted("{}\n{}\n{}\n", default_query, fixed_width_query, window_title_query));
}

ByteString host_path_for_resource(ByteString const& path)
{
    return LibWM::Environment::host_resource_path(path);
}

}

namespace LibWM {

WindowServerConnection::FontQueries WindowServerConnection::read_fonts_broker()
{
    FontQueries queries;
    auto file = Core::File::open(fonts_broker_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return queries;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return queries;
    auto lines = StringView { contents.value().bytes() }.split_view('\n');
    if (lines.size() > 0)
        queries.default_query = lines[0].trim_whitespace().to_byte_string();
    if (lines.size() > 1)
        queries.fixed_width_query = lines[1].trim_whitespace().to_byte_string();
    if (lines.size() > 2)
        queries.window_title_query = lines[2].trim_whitespace().to_byte_string();
    return queries;
}

Messages::WindowServer::GetWallpaperResponse WindowServerConnection::get_wallpaper()
{
    if (m_wallpaper)
        return m_wallpaper->to_shareable_bitmap();
    // Rolling-upgrade compatibility: an older compositor still pushes the
    // wallpaper over serenity-window. New compositors leave it unset.
    if (auto host = WaylandClient::the().serenity_shell().host_wallpaper()) {
        m_wallpaper = host;
        return m_wallpaper->to_shareable_bitmap();
    }
    // The wallpaper is normally loaded on demand from the compositor's persisted
    // raw state. Only the desktop/Display Settings ask for it, so the rest of the
    // session does not pay for a private copy of the image.
    m_wallpaper = read_wallpaper_state();
    if (m_wallpaper)
        return m_wallpaper->to_shareable_bitmap();
    return Gfx::ShareableBitmap {};
}

Messages::WindowServer::SetWallpaperResponse WindowServerConnection::set_wallpaper(Gfx::ShareableBitmap const& wallpaper_bitmap)
{
    m_wallpaper = wallpaper_bitmap.bitmap();
    // The desktop owns the wallpaper: hand the raw pixels over (no PNG encode,
    // which would stall this application's UI thread).
    WaylandClient::the().serenity_shell().set_host_wallpaper(m_wallpaper.ptr());
    return true;
}

void WindowServerConnection::set_wallpaper_mode(ByteString const& mode)
{
    m_wallpaper_mode = mode;
    WaylandClient::the().serenity_shell().set_host_setting("wallpaper.mode"sv, mode);
}

void WindowServerConnection::set_background_color(ByteString const& background_color)
{
    WaylandClient::the().serenity_shell().set_host_setting("wallpaper.color"sv, background_color);
}

Messages::WindowServer::GetSystemThemeResponse WindowServerConnection::get_system_theme()
{
    return m_theme_name;
}

Messages::WindowServer::SetSystemThemeResponse WindowServerConnection::set_system_theme(ByteString const& theme_path, ByteString const& theme_name, bool, Optional<ByteString> const& color_scheme_path)
{
    auto host_theme = host_path_for_resource(theme_path);
    Optional<ByteString> host_scheme;
    if (color_scheme_path.has_value())
        host_scheme = host_path_for_resource(color_scheme_path.value());

    auto buffer = Gfx::load_system_theme(host_theme, host_scheme);
    if (buffer.is_error())
        return false;

    m_theme_name = theme_name;
    // Restyle this client, then record the choice so other clients and the
    // compositor pick it up from the shared broker file.
    async_update_system_theme(buffer.value());
    if (auto file = Core::File::open(theme_broker_path(), Core::File::OpenMode::Write); !file.is_error())
        (void)file.value()->write_until_depleted(host_theme);
    // Persist the choice so it survives a restart (the compositor applies [Theme]
    // at startup).
    WaylandClient::the().serenity_shell().set_host_setting("theme.name"sv, theme_name);
    WaylandClient::the().serenity_shell().set_host_setting("theme.path"sv, host_theme);
    return true;
}

void WindowServerConnection::watch_theme()
{
    if (m_theme_watcher)
        return;
    // Apply any already-chosen theme immediately (a client started after a theme
    // change, or a fresh session with a persisted [Theme], must pick it up), then
    // follow further changes.
    apply_theme_from_broker();
    m_theme_watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    m_theme_watcher->on_change = [this](Core::FileWatcherEvent const& event) {
        if (event.event_path != theme_broker_path())
            return;
        apply_theme_from_broker();
    };
    // Watch the runtime directory so creation of the file is seen too, and the
    // file itself (a write is IN_MODIFY, which a directory watch does not report).
    if (auto directory = LexicalPath(theme_broker_path()).dirname().to_byte_string(); !directory.is_empty())
        (void)m_theme_watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    auto path = theme_broker_path();
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)m_theme_watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

void WindowServerConnection::apply_theme_from_broker()
{
    ByteString host_theme;
    bool from_broker = false;
    if (auto file = Core::File::open(theme_broker_path(), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            host_theme = StringView { contents.value() }.trim_whitespace().to_byte_string();
            from_broker = !host_theme.is_empty();
        }
    }
    // Fall back to the persisted theme ([Theme]) when there is no live broker value.
    if (host_theme.is_empty())
        host_theme = WaylandClient::the().serenity_shell().host_setting("theme.path"sv, ""sv);
    if (host_theme.is_empty())
        return;
    if (auto buffer = Gfx::load_system_theme(host_theme, {}); !buffer.is_error()) {
        auto name = WaylandClient::the().serenity_shell().host_setting("theme.name"sv, ""sv);
        m_theme_name = name.is_empty() ? ByteString { LexicalPath(host_theme).title() } : name;
        // Say which theme this client settled on and where it heard about it: an
        // empty theme name is what made Display Settings' Themes page show a theme
        // other than the running one, and it was invisible from the outside.
        trace("LibWM: system theme is '{}' (from {})", m_theme_name,
            from_broker ? "the broker"sv : "the host settings"sv);
        async_update_system_theme(buffer.value());
    }
}

void WindowServerConnection::watch_fonts()
{
    if (m_fonts_watcher)
        return;
    // Apply any already-chosen font immediately (a client started after a font
    // change must still pick it up), then follow further changes.
    apply_fonts_from_broker();
    m_fonts_watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    m_fonts_watcher->on_change = [this](Core::FileWatcherEvent const& event) {
        if (event.event_path != fonts_broker_path())
            return;
        apply_fonts_from_broker();
    };
    if (auto directory = LexicalPath(fonts_broker_path()).dirname().to_byte_string(); !directory.is_empty())
        (void)m_fonts_watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    auto path = fonts_broker_path();
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)m_fonts_watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

void WindowServerConnection::apply_fonts_from_broker()
{
    auto queries = read_fonts_broker();
    if (queries.default_query.is_empty())
        return;
    trace("LibWM: applying font broker (default '{}')", queries.default_query);
    // The connection runs in-process with the client, so set the queries directly
    // too: widgets created afterwards (e.g. Display Settings' Fonts tab) then see
    // the chosen font, not just those relayouted by the async notification.
    Gfx::FontDatabase::set_default_font_query(queries.default_query);
    if (!queries.fixed_width_query.is_empty())
        Gfx::FontDatabase::set_fixed_width_font_query(queries.fixed_width_query);
    if (!queries.window_title_query.is_empty())
        Gfx::FontDatabase::set_window_title_font_query(queries.window_title_query);
    async_update_system_fonts(queries.default_query, queries.fixed_width_query, queries.window_title_query);
}

Messages::WindowServer::IsSystemThemeOverriddenResponse WindowServerConnection::is_system_theme_overridden()
{
    return m_theme_override.has_value();
}

Messages::WindowServer::SetSystemThemeOverrideResponse WindowServerConnection::set_system_theme_override(Core::AnonymousBuffer const& buffer)
{
    if (!buffer.is_valid())
        return false;
    if (!m_theme_override.has_value())
        m_theme_before_override = Gfx::current_system_theme_buffer();
    m_theme_override = buffer;
    async_update_system_theme(buffer);
    trace("LibWM: system theme override applied");
    return true;
}

void WindowServerConnection::clear_system_theme_override()
{
    if (!m_theme_override.has_value())
        return;
    m_theme_override.clear();
    if (m_theme_before_override.has_value())
        async_update_system_theme(m_theme_before_override.release_value());
    trace("LibWM: system theme override cleared");
}

Messages::WindowServer::GetPreferredColorSchemeResponse WindowServerConnection::get_preferred_color_scheme()
{
    return ByteString { "Default" };
}

Messages::WindowServer::GetScreenLayoutResponse WindowServerConnection::get_screen_layout()
{
    // The desktop owns the output layout; it is served over serenity-window
    // as "screenlayout" (main=<i>;<Mode>,<Device>,<L>,<T>,<W>,<H>,<Scale>;...).
    auto encoded = WaylandClient::the().serenity_shell().host_setting("screenlayout"sv);
    if (!encoded.is_empty()) {
        WindowServer::ScreenLayout parsed;
        for (auto field : encoded.split_view(';')) {
            if (field.starts_with("main="sv)) {
                parsed.main_screen_index = field.substring_view(5).to_number<unsigned>().value_or(0);
                continue;
            }
            auto parts = field.split_view(',');
            if (parts.size() < 7)
                continue;
            auto mode = WindowServer::ScreenLayout::Screen::Mode::Invalid;
            if (parts[0] == "Device"sv)
                mode = WindowServer::ScreenLayout::Screen::Mode::Device;
            else if (parts[0] == "Virtual"sv)
                mode = WindowServer::ScreenLayout::Screen::Mode::Virtual;
            if (mode == WindowServer::ScreenLayout::Screen::Mode::Invalid)
                continue;
            Optional<ByteString> device;
            if (mode == WindowServer::ScreenLayout::Screen::Mode::Device && !parts[1].is_empty())
                device = parts[1].to_byte_string();
            parsed.screens.append(WindowServer::ScreenLayout::Screen {
                .mode = mode,
                .device = move(device),
                .location = { parts[2].to_number<int>().value_or(0), parts[3].to_number<int>().value_or(0) },
                .resolution = { parts[4].to_number<int>().value_or(0), parts[5].to_number<int>().value_or(0) },
                .scale_factor = parts[6].to_number<int>().value_or(1),
            });
        }
        if (!parsed.screens.is_empty())
            return parsed;
    }

    WindowServer::ScreenLayout layout;
    WindowServer::ScreenLayout::Screen screen {
        .mode = WindowServer::ScreenLayout::Screen::Mode::Device,
        .device = ByteString { "sde-0" },
        .location = { 0, 0 },
        .resolution = m_screen_size,
        .scale_factor = 1,
    };
    layout.screens.append(move(screen));
    layout.main_screen_index = 0;
    return layout;
}

Messages::WindowServer::SetScreenLayoutResponse WindowServerConnection::set_screen_layout(WindowServer::ScreenLayout const& screen_layout, bool save)
{
    // The desktop owns the output layout, so hand it over (the compositor
    // applies and persists it) rather than writing its screenlayout.ini.
    (void)save;
    StringBuilder encoded;
    encoded.appendff("main={}", screen_layout.main_screen_index);
    for (auto const& screen : screen_layout.screens)
        encoded.appendff(";{},{},{},{},{},{},{}", WindowServer::ScreenLayout::Screen::mode_to_string(screen.mode),
            screen.device.value_or(ByteString {}), screen.location.x(), screen.location.y(),
            screen.resolution.width(), screen.resolution.height(), screen.scale_factor);
    WaylandClient::the().serenity_shell().set_host_setting("screenlayout"sv, encoded.string_view());
    return { true, {} };
}

Messages::WindowServer::SaveScreenLayoutResponse WindowServerConnection::save_screen_layout()
{
    return { true, {} };
}

Messages::WindowServer::GetDesktopDisplayScaleResponse WindowServerConnection::get_desktop_display_scale(u32)
{
    return WaylandClient::the().outputs().output_scale();
}

void WindowServerConnection::set_output_scale(i32 scale_v120)
{
    // HiDPI: the DisplaySettings scale control calls this; we write the
    // compositor's [Output] DefaultScale, which it live-applies. v120 carries a fractional scale exactly (1.5 -> 180).
    if (scale_v120 < 60) // ignore nonsense (< 0.5x)
        return;
    double const scale = static_cast<double>(scale_v120) / 120.0;
    // The desktop owns the output scale; hand it over (the compositor applies and
    // persists it) rather than writing its config.
    WaylandClient::the().serenity_shell().set_host_setting("output.default_scale"sv, ByteString::formatted("{}", scale));
    trace("LibWM: set output scale {}x", scale);
}

// --- mouse/pointer settings -----------------------------------------------

void WindowServerConnection::set_mouse_acceleration(float factor)
{
    WaylandClient::the().serenity_shell().set_host_setting("mouse.acceleration"sv, ByteString::formatted("{}", factor));
}

Messages::WindowServer::GetMouseAccelerationResponse WindowServerConnection::get_mouse_acceleration()
{
    return WaylandClient::the().serenity_shell().host_setting("mouse.acceleration"sv, "1.0"sv).to_number<double>().value_or(1.0);
}

void WindowServerConnection::set_scroll_step_size(u32 step_size)
{
    WaylandClient::the().serenity_shell().set_host_setting("mouse.scroll_step_size"sv, ByteString::number(step_size));
}

Messages::WindowServer::GetScrollStepSizeResponse WindowServerConnection::get_scroll_step_size()
{
    return WaylandClient::the().serenity_shell().host_setting("mouse.scroll_step_size"sv, "1"sv).to_number<u32>().value_or(1);
}

void WindowServerConnection::set_double_click_speed(int speed)
{
    m_double_click_speed = speed;
    WaylandClient::the().serenity_shell().set_host_setting("mouse.double_click_speed"sv, ByteString::number(speed));
}

Messages::WindowServer::GetDoubleClickSpeedResponse WindowServerConnection::get_double_click_speed()
{
    m_double_click_speed = WaylandClient::the().serenity_shell().host_setting("mouse.double_click_speed"sv, "250"sv).to_number<int>().value_or(250);
    return m_double_click_speed;
}

void WindowServerConnection::set_mouse_buttons_switched(bool switched)
{
    WaylandClient::the().serenity_shell().set_host_setting("mouse.buttons_switched"sv, switched ? "true"sv : "false"sv);
}

Messages::WindowServer::AreMouseButtonsSwitchedResponse WindowServerConnection::are_mouse_buttons_switched()
{
    return WaylandClient::the().serenity_shell().host_setting("mouse.buttons_switched"sv, "false"sv) == "true"sv;
}

void WindowServerConnection::set_natural_scroll(bool inverted)
{
    WaylandClient::the().serenity_shell().set_host_setting("mouse.natural_scroll"sv, inverted ? "true"sv : "false"sv);
}

Messages::WindowServer::IsNaturalScrollResponse WindowServerConnection::is_natural_scroll()
{
    return WaylandClient::the().serenity_shell().host_setting("mouse.natural_scroll"sv, "false"sv) == "true"sv;
}

Messages::WindowServer::GetMouseAccelerationMinResponse WindowServerConnection::get_mouse_acceleration_min()
{
    return static_cast<float>(WaylandClient::the().serenity_shell().host_config().accel_min);
}

Messages::WindowServer::GetMouseAccelerationMaxResponse WindowServerConnection::get_mouse_acceleration_max()
{
    return static_cast<float>(WaylandClient::the().serenity_shell().host_config().accel_max);
}

Messages::WindowServer::GetScrollStepSizeMinResponse WindowServerConnection::get_scroll_step_size_min()
{
    return WaylandClient::the().serenity_shell().host_config().scroll_step_min;
}

Messages::WindowServer::GetDoubleClickSpeedMinResponse WindowServerConnection::get_double_click_speed_min()
{
    return WaylandClient::the().serenity_shell().host_config().double_click_min;
}

Messages::WindowServer::GetDoubleClickSpeedMaxResponse WindowServerConnection::get_double_click_speed_max()
{
    return WaylandClient::the().serenity_shell().host_config().double_click_max;
}

Messages::WindowServer::GetCursorThemeResponse WindowServerConnection::get_cursor_theme()
{
    return WaylandClient::the().serenity_shell().host_setting("cursor.theme"sv, "Default"sv);
}

void WindowServerConnection::apply_cursor_theme(ByteString const& name)
{
    WaylandClient::the().serenity_shell().set_host_setting("cursor.theme"sv, name);
}

Messages::WindowServer::GetCursorHighlightRadiusResponse WindowServerConnection::get_cursor_highlight_radius()
{
    return WaylandClient::the().serenity_shell().host_setting("cursor.highlight_radius"sv, "0"sv).to_number<int>().value_or(0);
}

void WindowServerConnection::set_cursor_highlight_radius(int radius)
{
    WaylandClient::the().serenity_shell().set_host_setting("cursor.highlight_radius"sv, ByteString::number(radius));
}

Messages::WindowServer::GetCursorHighlightColorResponse WindowServerConnection::get_cursor_highlight_color()
{
    if (auto color = Gfx::Color::from_string(WaylandClient::the().serenity_shell().host_setting("cursor.highlight_color"sv, "#000000"sv)); color.has_value())
        return color.value();
    return Gfx::Color::Black;
}

void WindowServerConnection::set_cursor_highlight_color(Gfx::Color color)
{
    WaylandClient::the().serenity_shell().set_host_setting("cursor.highlight_color"sv, color.to_byte_string());
}

Messages::WindowServer::GetKeyboardLayoutResponse WindowServerConnection::get_keyboard_layout()
{
    return { WaylandClient::the().serenity_shell().host_setting("keyboard.layouts"sv, "us"sv),
        WaylandClient::the().serenity_shell().host_setting("keyboard.variant"sv, ""sv),
        WaylandClient::the().serenity_shell().host_setting("keyboard.model"sv, "pc105"sv),
        WaylandClient::the().serenity_shell().host_setting("keyboard.options"sv, ""sv) };
}

void WindowServerConnection::set_keyboard_layout(ByteString const& layouts, ByteString const& variant, ByteString const& model, ByteString const& options)
{
    WaylandClient::the().serenity_shell().set_host_setting("keyboard.layouts"sv, layouts);
    WaylandClient::the().serenity_shell().set_host_setting("keyboard.variant"sv, variant);
    WaylandClient::the().serenity_shell().set_host_setting("keyboard.model"sv, model);
    WaylandClient::the().serenity_shell().set_host_setting("keyboard.options"sv, options);
}

Messages::WindowServer::GetKeyboardLayoutNamesResponse WindowServerConnection::get_keyboard_layout_names()
{
    Vector<ByteString> names;
    // Prefer the canonical xkb list: the [! layout] section of evdev.lst.
    if (auto file = Core::File::open("/usr/share/X11/xkb/rules/evdev.lst"sv, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            bool in_layout = false;
            StringView text { contents.value().bytes() };
            for (auto line : text.split_view('\n')) {
                if (line.starts_with("! "sv)) {
                    in_layout = line.starts_with("! layout"sv);
                    continue;
                }
                if (!in_layout)
                    continue;
                auto trimmed = line.trim_whitespace(TrimMode::Left);
                if (trimmed.is_empty())
                    continue;
                names.append(trimmed.split_view(' ').first());
            }
        }
    }
    if (names.is_empty()) {
        (void)Core::Directory::for_each_entry("/usr/share/X11/xkb/symbols"sv, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto&) -> ErrorOr<IterationDecision> {
            if (!entry.name.starts_with("_"sv))
                names.append(entry.name);
            return IterationDecision::Continue;
        });
    }
    names.append("us"sv);
    quick_sort(names);
    Vector<ByteString> unique;
    for (auto& name : names) {
        if (unique.is_empty() || unique.last() != name)
            unique.append(move(name));
    }
    return unique;
}

void WindowServerConnection::set_system_effects(Vector<bool> const& effects, u8 geometry, u8 tile_window)
{
    // Apply to this client (LibGUI's SystemEffects) and record the choices so the
    // compositor (geometry HUD) and other clients pick them up.
    async_update_system_effects(effects);
    WaylandClient::the().serenity_shell().set_host_setting("effects.show_geometry"sv, WindowServer::ShowGeometryTools::enum_to_string(static_cast<WindowServer::ShowGeometry>(geometry)));
    WaylandClient::the().serenity_shell().set_host_setting("effects.tile_window"sv, WindowServer::TileWindowTools::enum_to_string(static_cast<WindowServer::TileWindow>(tile_window)));
    static constexpr Array effect_names = {
        "AnimateMenus"sv, "FlashMenus"sv, "AnimateWindows"sv, "SmoothScrolling"sv,
        "TabAccents"sv, "SplitterKnurls"sv, "Tooltips"sv, "MenuShadow"sv,
        "WindowShadow"sv, "TooltipShadow"sv,
    };
    for (size_t i = 0; i < effects.size() && i < effect_names.size(); ++i)
        WaylandClient::the().serenity_shell().set_host_setting(ByteString::formatted("effects.{}", effect_names[i]), effects[i] ? "true"sv : "false"sv);
}

Messages::WindowServer::SetSystemFontsResponse WindowServerConnection::set_system_fonts(ByteString const& default_font_query, ByteString const& fixed_width_font_query, ByteString const& window_title_font_query)
{
    // Apply in-process (so subsequent lookups resolve the new font), restyle this
    // client and tell it to re-layout, then record the choice in the session font
    // broker so the compositor and other clients pick it up.
    Gfx::FontDatabase::set_default_font_query(default_font_query);
    if (!fixed_width_font_query.is_empty())
        Gfx::FontDatabase::set_fixed_width_font_query(fixed_width_font_query);
    if (!window_title_font_query.is_empty())
        Gfx::FontDatabase::set_window_title_font_query(window_title_font_query);
    async_update_system_fonts(default_font_query, fixed_width_font_query, window_title_font_query);
    write_fonts_broker(default_font_query, fixed_width_font_query, window_title_font_query);
    return true;
}

Messages::WindowServer::ApplyWorkspaceSettingsResponse WindowServerConnection::apply_workspace_settings(u32 rows, u32 columns, bool save)
{
    // Write the grid to the session config; the compositor live-reloads it and the
    // workspace picker follows. `save` is implied: the file is the store.
    (void)save;
    if (rows < 1 || columns < 1)
        return false;
    WaylandClient::the().serenity_shell().set_host_setting("workspaces.rows"sv, ByteString::number(rows));
    WaylandClient::the().serenity_shell().set_host_setting("workspaces.columns"sv, ByteString::number(columns));
    return true;
}

Messages::WindowServer::GetWorkspaceSettingsResponse WindowServerConnection::get_workspace_settings()
{
    auto cfg = WaylandClient::the().serenity_shell().host_config();
    return { cfg.workspace_rows, cfg.workspace_columns, 8u, 8u };
}

Messages::WindowServer::GetSystemThemeOverrideResponse WindowServerConnection::get_system_theme_override()
{
    // Must be a valid response: the theme-change handler calls this synchronously.
    return m_theme_override;
}

}
