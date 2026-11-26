/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The menubar target for the menu suite. The applications that used to provide
// it (Calculator) are built by the desktop now, so this test app keeps the same
// menu structure: File -> Quit first, Help with About as its third item, and a
// few menus in between so the Help item lands where the script clicks. The About
// item opens a second window; the suite checks that it does.

#include "LibWM.h"
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Window.h>
#include <LibGUI/Widget.h>
#include <LibMain/Main.h>

namespace {

RefPtr<GUI::Window> s_about_window;

void show_about_window()
{
    if (s_about_window)
        return;
    s_about_window = GUI::Window::construct();
    s_about_window->set_title("About LibWM Menu Test");
    s_about_window->set_rect(180, 180, 260, 120);
    auto label = s_about_window->set_main_widget<GUI::Label>();
    label->set_text("LibWM menu test"_string);
    label->set_text_alignment(Gfx::TextAlignment::Center);
    s_about_window->show();
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM Menu Test");
    window->set_rect(100, 100, 250, 215);
    window->set_resizable(false);
    auto widget = window->set_main_widget<GUI::Widget>();
    widget->set_fill_with_background_color(true);

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit(0);
    }));

    auto edit_menu = window->add_menu("&Edit"_string);
    edit_menu->add_action(GUI::Action::create("&Copy", [](auto&) { }));
    edit_menu->add_action(GUI::Action::create("&Paste", [](auto&) { }));

    auto constants_menu = window->add_menu("&Constants"_string);
    for (auto name : { "&Pi"sv, "&Euler's Number"sv, "&Phi"sv })
        constants_menu->add_action(GUI::Action::create(name, [](auto&) { }));

    auto round_menu = window->add_menu("&Round"_string);
    for (auto name : { "To &0 Digits"sv, "To &2 Digits"sv, "To &4 Digits"sv })
        round_menu->add_action(GUI::Action::create(name, [](auto&) { }));

    auto view_menu = window->add_menu("&View"_string);
    view_menu->add_action(GUI::CommonActions::make_fullscreen_action([&](auto&) {
        window->set_fullscreen(!window->is_fullscreen());
    }));

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::Action::create("&Command Palette", [](auto&) { }));
    help_menu->add_action(GUI::Action::create("&Help", [](auto&) { }));
    help_menu->add_action(GUI::Action::create("&About", [](auto&) { show_about_window(); }));

    window->show();
    return app->exec();
}
