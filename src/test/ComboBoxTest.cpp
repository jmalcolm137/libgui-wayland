/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

// Regression test for a ComboBox dropdown (e.g. the Display Settings wallpaper
// "Mode" box): the popup must open at a sensible size, not collapse. The driver
// (serenity-desktop-environment/scripts/run-combo-test.sh) opens the popup and
// asserts the popup layer surface's geometry from the compositor log.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    int x = 200;
    int y = 100;
    if (arguments.strings.size() >= 3) {
        x = arguments.strings[1].to_number<int>().value_or(x);
        y = arguments.strings[2].to_number<int>().value_or(y);
    }

    auto window = GUI::Window::construct();
    window->set_title("ComboBox Test");
    window->set_rect(x, y, 320, 180);
    auto widget = window->set_main_widget<GUI::Widget>();
    auto& combo = widget->add<GUI::ComboBox>();
    combo.set_relative_rect(20, 20, 160, 24);
    Vector<String> items;
    items.append("Centered"_string);
    items.append("Expand"_string);
    items.append("Tile"_string);
    items.append("Fill"_string);
    combo.set_model(*GUI::ItemListModel<String>::create(move(items)));
    window->show();

    auto open_timer = Core::Timer::create_single_shot(700, [&] {
        dbgln("COMBO-TEST: opening popup at {}", combo.screen_relative_rect());
        combo.open();
    });
    open_timer->start();

    auto quit = Core::Timer::create_single_shot(2000, [&] { app->quit(0); });
    quit->start();
    return app->exec();
}
