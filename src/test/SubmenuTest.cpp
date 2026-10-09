/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Submenu Test");
    window->set_rect(100, 100, 300, 200);
    auto label = window->set_main_widget<GUI::Label>();
    label->set_text("Submenu Test"_string);
    label->set_fill_with_background_color(true);

    auto file_menu = window->add_menu("&File"_string);
    auto new_menu = file_menu->add_submenu("&New"_string);
    new_menu->add_action(GUI::Action::create("&Project", [](auto&) {
        dbgln("SUBMENU-TEST: activated Project");
        GUI::Application::the()->quit();
    }));
    new_menu->add_action(GUI::Action::create("&From Template", [](auto&) { dbgln("SUBMENU-TEST: activated From Template"); }));
    file_menu->add_separator();
    file_menu->add_action(GUI::CommonActions::make_quit_action([](auto&) { GUI::Application::the()->quit(); }));

    window->show();

    auto quit_timer = Core::Timer::create_single_shot(6000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
