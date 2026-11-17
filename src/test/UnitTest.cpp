/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardServerConnection.h"
#include "ConfigServerConnection.h"
#include "LibWM.h"
#include "MenuController.h"
#include <AK/Format.h>
#include <Kernel/API/KeyCode.h>
#include <LibConfig/Client.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibMain/Main.h>

namespace {

int s_passed = 0;
int s_failed = 0;

void check(bool condition, StringView name)
{
    if (condition) {
        outln("PASS: {}", name);
        ++s_passed;
    } else {
        outln("FAIL: {}", name);
        ++s_failed;
    }
}

void test_menu_controller()
{
    LibWM::MenuController controller;

    Vector<i32> shown_popups;
    Vector<i32> hidden_popups;
    i32 last_parent_popup = -1;
    bool last_is_submenu = false;
    i32 activated_menu = -1;
    u32 activated_id = 0;
    Vector<u32> entered_items;
    Vector<u32> left_items;

    controller.show_popup = [&](i32 menu_id, i32, i32 parent_popup_id, Gfx::IntRect, Gfx::IntSize, bool is_submenu, bool) {
        shown_popups.append(menu_id);
        last_parent_popup = parent_popup_id;
        last_is_submenu = is_submenu;
    };
    controller.hide_popup = [&](i32 menu_id) {
        hidden_popups.append(menu_id);
    };
    controller.item_activated = [&](i32 menu_id, u32 identifier) {
        activated_menu = menu_id;
        activated_id = identifier;
    };
    controller.item_entered = [&](i32, u32 identifier) {
        entered_items.append(identifier);
    };
    controller.item_left = [&](i32, u32 identifier) {
        left_items.append(identifier);
    };

    // Build a menubar for window 1:
    //   Menu 10 ("&File"):
    //     [0] id=101 "&New" -> submenu 30
    //     [1] separator
    //     [2] id=102 "Disabled" (enabled=false)
    //     [3] id=103 "&Quit"
    //   Menu 20 ("R&&D &Edit"): (escaped '&&' before '&E')
    //     [0] id=201 "&Copy"
    //   Submenu 30 ("New"):
    //     [0] id=301 "&Project"
    //     [1] id=302 "&File"
    controller.create_menu(10, "&File", 0);
    controller.create_menu(20, "R&&D &Edit", 0);
    controller.create_menu(30, "New", 0);

    controller.add_menu_item(30, 301, -1, "&Project", true, true, false, false, false, "", nullptr, false);
    controller.add_menu_item(30, 302, -1, "&File", true, true, true, true, false, "Ctrl+N", nullptr, false);

    controller.add_menu_item(10, 101, 30, "&New", true, true, false, false, false, "", nullptr, false);
    controller.add_menu_separator(10);
    controller.add_menu_item(10, 102, -1, "Disabled", false, true, false, false, false, "", nullptr, false);
    controller.add_menu_item(10, 103, -1, "&Quit", true, true, false, false, false, "Ctrl+Q", nullptr, false);

    controller.add_menu_item(20, 201, -1, "&Copy", true, true, false, false, false, "Ctrl+C", nullptr, false);

    check(!controller.window_has_menubar(1), "window_has_menubar false before add_menu"sv);
    controller.add_menu(1, 10);
    controller.add_menu(1, 20);
    check(controller.window_has_menubar(1), "window_has_menubar true after add_menu"sv);
    check(controller.menubar_height() >= 20, "menubar_height at least 20px"sv);

    // Render menubar to compute menubar_rect geometry.
    auto menubar_bmp = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 400, controller.menubar_height() }));
    {
        Gfx::Painter painter(*menubar_bmp);
        controller.render_menubar(1, painter, menubar_bmp->rect());
    }

    // F10 opens the first menubar menu (10).
    check(!controller.has_open_menu(), "no menu open initially"sv);
    bool handled = controller.handle_key(1, Key_F10, 0, Mod_None, true);
    check(handled && controller.has_open_menu(), "F10 opens first menubar menu"sv);
    check(shown_popups.size() == 1 && shown_popups.last() == 10 && !last_is_submenu, "show_popup fired for root menu 10"sv);

    auto popup10_size = controller.popup_size(10);
    check(popup10_size.width() > 40 && popup10_size.height() > 40, "popup 10 layout has non-empty size"sv);
    auto popup10_bmp = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, popup10_size));
    {
        Gfx::Painter painter(*popup10_bmp);
        controller.render_popup(10, painter);
    }

    // Down selects item 0 (id=101, "&New").
    controller.handle_key(1, Key_Down, 0, Mod_None, true);
    check(!entered_items.is_empty() && entered_items.last() == 101, "Key_Down selects first enabled item (101)"sv);

    // Next Down must skip both the separator [1] and the disabled item [2], landing on [3] (id=103, "&Quit").
    controller.handle_key(1, Key_Down, 0, Mod_None, true);
    check(entered_items.last() == 103 && left_items.last() == 101, "Key_Down skips separator and disabled item to select 103"sv);

    // Up goes back to 101 ("&New").
    controller.handle_key(1, Key_Up, 0, Mod_None, true);
    check(entered_items.last() == 101, "Key_Up returns to item 101"sv);

    // Right on submenu item 101 opens submenu 30 and selects its first item (301).
    controller.handle_key(1, Key_Right, 0, Mod_None, true);
    check(shown_popups.last() == 30 && last_parent_popup == 10 && last_is_submenu, "Key_Right opens child submenu 30 parented to 10"sv);
    check(entered_items.last() == 301, "Key_Right selects first item (301) in submenu 30"sv);

    // Left closes submenu 30 and keeps root menu 10 open.
    controller.handle_key(1, Key_Left, 0, Mod_None, true);
    check(!hidden_popups.is_empty() && hidden_popups.last() == 30 && controller.has_open_menu(), "Key_Left closes submenu 30 while keeping root menu open"sv);

    // Alt+E switches to menu 20 ("R&&D &Edit" -> accelerator is 'e', ignoring '&&').
    controller.handle_key(1, Key_E, 'e', Mod_Alt, true);
    check(shown_popups.last() == 20 && !last_is_submenu, "Alt+E switches to menu 20 despite escaped &&"sv);

    // Down + Return activates item 201 ("&Copy") and closes all menus.
    controller.handle_key(1, Key_Down, 0, Mod_None, true);
    controller.handle_key(1, Key_Return, '\n', Mod_None, true);
    check(activated_menu == 20 && activated_id == 201, "Key_Return activates selected item 201"sv);
    check(!controller.has_open_menu(), "menus closed after item activation"sv);

    // Pointer interaction: clicking menubar at (10, 5) opens menu 10; hovering submenu item opens 30;
    // releasing button on submenu item 302 activates it.
    controller.on_menubar_press(1, { 10, 5 });
    check(controller.has_open_menu() && shown_popups.last() == 10, "on_menubar_press opens menu 10"sv);
    controller.on_popup_motion(10, { 15, 10 });
    check(shown_popups.last() == 30 && last_is_submenu, "on_popup_motion over submenu item opens submenu 30"sv);
    auto sub_size = controller.popup_size(30);
    controller.on_popup_button(30, { 15, sub_size.height() - 8 }, false);
    check(activated_menu == 30 && activated_id == 302 && !controller.has_open_menu(), "on_popup_button release activates submenu item 302 and closes menus"sv);

    // Separator removal when rebuilding menus: separators carry no identifier in
    // the protocol, so an unmatched remove_menu_item clears all separators.
    auto height_with_sep = controller.popup_size(10).height();
    controller.remove_menu_item(10, 99999);
    controller.open_root(10, 1, { 0, 0, 40, 20 }, true);
    auto height_without_sep = controller.popup_size(10).height();
    check(height_without_sep < height_with_sep, "unmatched remove_menu_item drops separators from menu"sv);
    controller.close_open_menus();

    // Item::visible: hidden items do not contribute to popup height and are skipped
    // by keyboard navigation.
    controller.create_menu(40, "&View", 0);
    controller.add_menu_item(40, 401, -1, "Hidden First", true, false, false, false, false, "", nullptr, false);
    controller.add_menu_item(40, 402, -1, "Visible Second", true, true, false, false, false, "", nullptr, false);
    controller.add_menu_item(40, 403, -1, "Visible Third", true, true, false, false, false, "", nullptr, false);
    controller.open_root(40, 1, { 0, 0, 40, 20 }, false);
    auto height_two_visible = controller.popup_size(40).height();
    controller.handle_key(1, Key_Down, 0, Mod_None, true);
    check(entered_items.last() == 402, "Key_Down skips hidden item (visible=false) to select 402"sv);
    controller.update_menu_item(40, 403, -1, "Visible Third", true, false, false, false, false, "", nullptr);
    check(controller.popup_size(40).height() < height_two_visible, "hiding item via update_menu_item shrinks popup height"sv);
    controller.close_open_menus();

    // Long-menu scrolling: clamp popup height to max_popup_height, scroll on
    // Key_Down and on_popup_wheel, and hit-test scrolled items accurately.
    controller.set_max_popup_height(60);
    controller.create_menu(50, "&Bookmarks", 0);
    for (int i = 1; i <= 10; ++i)
        controller.add_menu_item(50, 500 + i, -1, ByteString::formatted("Bookmark {}", i), true, true, false, false, false, "", nullptr, false);
    controller.open_root(50, 1, { 0, 0, 40, 20 }, false);
    check(controller.popup_size(50).height() == 60, "long menu popup height clamped to max_popup_height (60px)"sv);
    check(controller.popup_scroll_offset(50) == 0, "long menu starts at scroll_offset 0"sv);
    for (int i = 0; i < 6; ++i)
        controller.handle_key(1, Key_Down, 0, Mod_None, true);
    check(controller.popup_scroll_offset(50) > 0, "Key_Down past viewport bottom scrolls long menu down"sv);
    int offset_after_keys = controller.popup_scroll_offset(50);
    controller.on_popup_wheel(50, 1);
    check(controller.popup_scroll_offset(50) > offset_after_keys, "on_popup_wheel(+1) scrolls long menu further down"sv);
    controller.on_popup_wheel(50, -10);
    // Reset to top via repeated wheel up
    for (int i = 0; i < 10; ++i)
        controller.on_popup_wheel(50, -1);
    check(controller.popup_scroll_offset(50) == 0, "on_popup_wheel(-1) scrolls long menu back to top"sv);
    controller.set_max_popup_height(0);
    controller.close_open_menus();
}

void test_clipboard_image_transcoding()
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 5, 4 }));
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < bitmap->width(); ++x)
            bitmap->set_pixel(x, y, Gfx::Color(static_cast<u8>(x * 40), static_cast<u8>(y * 60), 128, 255));
    }

    HashMap<ByteString, ByteString> metadata;
    metadata.set("width"sv, ByteString::number(bitmap->width()));
    metadata.set("height"sv, ByteString::number(bitmap->height()));
    metadata.set("scale"sv, ByteString::number(bitmap->scale()));
    metadata.set("pitch"sv, ByteString::number(bitmap->pitch()));
    metadata.set("format"sv, ByteString::number(static_cast<int>(bitmap->format())));

    ReadonlyBytes raw_bytes { bitmap->scanline(0), bitmap->size_in_bytes() };
    auto png_or_error = LibWM::serenity_image_to_png(raw_bytes, metadata);
    check(!png_or_error.is_error(), "serenity_image_to_png encodes valid BGRA8888 bitmap"sv);

    auto png = png_or_error.release_value();
    check(png.size() > 8 && png.bytes().slice(0, 4) == "\x89PNG"sv.bytes(), "encoded bytes start with PNG signature"sv);

    HashMap<ByteString, ByteString> out_metadata;
    auto decoded_or_error = LibWM::png_to_serenity_image(png.bytes(), out_metadata);
    check(!decoded_or_error.is_error(), "png_to_serenity_image decodes PNG back to raw bytes"sv);
    check(out_metadata.get("width"sv) == "5"sv && out_metadata.get("height"sv) == "4"sv, "decoded metadata preserves width=5 and height=4"sv);

    auto decoded_bytes = decoded_or_error.release_value();
    auto wrapped = MUST(Gfx::Bitmap::create_wrapper(
        static_cast<Gfx::BitmapFormat>(out_metadata.get("format"sv)->to_number<unsigned>().value()),
        { 5, 4 }, 1,
        out_metadata.get("pitch"sv)->to_number<unsigned>().value(),
        decoded_bytes.data()));
    check(wrapped->get_pixel(3, 2) == Gfx::Color(120, 120, 128, 255), "round-tripped pixel (3,2) matches exactly"sv);

    // Negative cases: missing metadata, invalid format, corrupt PNG.
    HashMap<ByteString, ByteString> incomplete = metadata;
    incomplete.remove("pitch"sv);
    check(LibWM::serenity_image_to_png(raw_bytes, incomplete).is_error(), "serenity_image_to_png rejects missing pitch metadata"sv);

    HashMap<ByteString, ByteString> bad_format = metadata;
    bad_format.set("format"sv, "9999"sv);
    check(LibWM::serenity_image_to_png(raw_bytes, bad_format).is_error(), "serenity_image_to_png rejects invalid bitmap format"sv);

    HashMap<ByteString, ByteString> dummy_meta;
    check(LibWM::png_to_serenity_image("not-a-png"sv.bytes(), dummy_meta).is_error(), "png_to_serenity_image rejects corrupt PNG bytes"sv);
}

void test_config_server_portal()
{
    Core::EventLoop loop;
    auto& client = Config::Client::the();

    // 1. Pre-seed an INI file on disk before querying "PreseededDomain" to verify
    //    lazy loading and typed parsing from disk.
    auto config_dir = LibWM::ConfigServerConnection::config_directory();
    (void)Core::Directory::create(config_dir, Core::Directory::CreateDirectories::Yes);
    auto preseeded_path = LibWM::ConfigServerConnection::domain_file_path("PreseededDomain");
    if (auto file = Core::File::open(preseeded_path, Core::File::OpenMode::Write); !file.is_error()) {
        (void)file.value()->write_until_depleted("[SavedGroup]\nTitle=FromDisk\nCount=-99\nSize=2048\nEnabled=true\n"sv);
    }

    check(client.read_string_value("PreseededDomain"sv, "SavedGroup"sv, "Title"sv) == "FromDisk"sv, "Config portal loads string from existing INI on disk"sv);
    check(client.read_i32_value("PreseededDomain"sv, "SavedGroup"sv, "Count"sv) == -99, "Config portal loads i32 from existing INI on disk"sv);
    check(client.read_u32_value("PreseededDomain"sv, "SavedGroup"sv, "Size"sv) == 2048u, "Config portal loads u32 from existing INI on disk"sv);
    check(client.read_bool_value("PreseededDomain"sv, "SavedGroup"sv, "Enabled"sv) == true, "Config portal loads bool from existing INI on disk"sv);

    // 2. Write values and verify both in-memory round-trip and disk persistence.
    client.write_string_value("TestDomain"sv, "Window"sv, "Title"sv, "Hello Wayland"sv);
    client.write_i32_value("TestDomain"sv, "Window"sv, "X"sv, -42);
    client.write_u32_value("TestDomain"sv, "Window"sv, "Width"sv, 1280u);
    client.write_bool_value("TestDomain"sv, "Window"sv, "Fullscreen"sv, true);

    check(client.read_string_value("TestDomain"sv, "Window"sv, "Title"sv) == "Hello Wayland"sv, "Config portal round-trips string value"sv);
    check(client.read_i32_value("TestDomain"sv, "Window"sv, "X"sv) == -42, "Config portal round-trips i32 value"sv);
    check(client.read_u32_value("TestDomain"sv, "Window"sv, "Width"sv) == 1280u, "Config portal round-trips u32 value"sv);
    check(client.read_bool_value("TestDomain"sv, "Window"sv, "Fullscreen"sv) == true, "Config portal round-trips bool value"sv);

    auto disk_cfg = Core::ConfigFile::open(LibWM::ConfigServerConnection::domain_file_path("TestDomain"));
    check(!disk_cfg.is_error()
            && disk_cfg.value()->read_entry("Window", "Title") == "Hello Wayland"sv
            && disk_cfg.value()->read_num_entry<i32>("Window", "X") == -42
            && disk_cfg.value()->read_num_entry<u32>("Window", "Width") == 1280u
            && disk_cfg.value()->read_bool_entry("Window", "Fullscreen") == true,
        "Config portal persists written domain to disk INI file"sv);

    // Type mismatch returns empty Optional rather than corrupt data.
    check(!client.read_i32_value("TestDomain"sv, "Window"sv, "Title"sv).has_value(), "Config portal returns empty Optional on type mismatch"sv);

    auto groups = client.list_config_groups("TestDomain"sv);
    check(groups.size() == 1 && groups[0] == "Window"sv, "Config portal lists groups"sv);

    auto keys = client.list_config_keys("TestDomain"sv, "Window"sv);
    check(keys.size() == 4, "Config portal lists 4 keys in group"sv);

    client.remove_key_entry("TestDomain"sv, "Window"sv, "X"sv);
    check(!client.read_i32_value("TestDomain"sv, "Window"sv, "X"sv).has_value(), "Config portal removes key entry"sv);

    client.remove_group_entry("TestDomain"sv, "Window"sv);
    check(client.list_config_groups("TestDomain"sv).is_empty(), "Config portal removes group entry"sv);
}

void test_wayland_client_thread_safety()
{
    auto& client = LibWM::WaylandClient::the();

    // Hammer WaylandClient state concurrently from 4 worker threads while the
    // main thread updates host settings, host config, scale, and wallpaper.
    constexpr int iterations = 500;
    Vector<pthread_t, 4> threads;
    for (int t = 0; t < 4; ++t) {
        pthread_t tid {};
        pthread_create(
            &tid, nullptr, +[](void*) -> void* {
                auto& c = LibWM::WaylandClient::the();
                for (int i = 0; i < iterations; ++i) {
                    c.set_host_setting("mouse.acceleration"sv, "1.5"sv);
                    (void)c.host_setting("mouse.acceleration"sv, "1.0"sv);
                    (void)c.host_config();
                    (void)c.host_wallpaper();
                    (void)c.captured_bitmap();
                    (void)c.clipboard_mime_types();
                    (void)c.window_menubar_visible(1);
                    (void)c.global_pointer_position();
                    c.set_client_scale((i % 2) + 1);
                    (void)c.client_scale();
                }
                return nullptr;
            },
            nullptr);
        threads.append(tid);
    }

    for (int i = 0; i < iterations; ++i) {
        client.on_host_settings("mouse.acceleration=2.0\ncursor.theme=Default\n"sv);
        client.on_host_config(2, 2, 0.5, 3.5, 1, 100, 900);
        client.on_output_scale((i % 2) + 1);
    }

    for (auto tid : threads)
        pthread_join(tid, nullptr);

    check(client.host_config().workspace_rows == 2 && client.host_config().workspace_columns == 2,
        "WaylandClient concurrent host_config updates are race-free"sv);
    check(!client.host_setting("mouse.acceleration"sv).is_empty(),
        "WaylandClient concurrent host_setting reads/writes are race-free"sv);
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    LibWM::initialize();

    test_menu_controller();
    test_clipboard_image_transcoding();
    test_config_server_portal();
    test_wayland_client_thread_safety();

    outln("==> libwm-unit-test: {} passed, {} failed", s_passed, s_failed);
    return s_failed == 0 ? 0 : 1;
}
