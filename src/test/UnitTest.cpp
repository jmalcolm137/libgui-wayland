/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardServerConnection.h"
#include "LibWM.h"
#include "MenuController.h"
#include <AK/Format.h>
#include <Kernel/API/KeyCode.h>
#include <LibConfig/Client.h>
#include <LibCore/EventLoop.h>
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

    client.write_string_value("TestDomain"sv, "Window"sv, "Title"sv, "Hello Wayland"sv);
    client.write_i32_value("TestDomain"sv, "Window"sv, "X"sv, -42);
    client.write_u32_value("TestDomain"sv, "Window"sv, "Width"sv, 1280u);
    client.write_bool_value("TestDomain"sv, "Window"sv, "Fullscreen"sv, true);

    check(client.read_string_value("TestDomain"sv, "Window"sv, "Title"sv) == "Hello Wayland"sv, "Config portal round-trips string value"sv);
    check(client.read_i32_value("TestDomain"sv, "Window"sv, "X"sv) == -42, "Config portal round-trips i32 value"sv);
    check(client.read_u32_value("TestDomain"sv, "Window"sv, "Width"sv) == 1280u, "Config portal round-trips u32 value"sv);
    check(client.read_bool_value("TestDomain"sv, "Window"sv, "Fullscreen"sv) == true, "Config portal round-trips bool value"sv);

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

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    LibWM::initialize();

    test_menu_controller();
    test_clipboard_image_transcoding();
    test_config_server_portal();

    outln("==> libwm-unit-test: {} passed, {} failed", s_passed, s_failed);
    return s_failed == 0 ? 0 : 1;
}
