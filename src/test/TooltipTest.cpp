/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

// Asks for a tooltip through GUI::Application::show_tooltip, so the host wiring
// (the Tooltips system effect from fast_greet plus the Application tooltip
// window/timer) and the compositor's WindowType::Tooltip semantics can be
// exercised end to end. See docs/05-r3-plan.md §4 WS6.
//
// The driver (serenity-desktop-environment/scripts/run-tooltip-test.sh) asserts
// that the compositor creates the tooltip as an overlay layer surface
// ("serenity-tooltip"), not a managed toplevel.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Tooltip Test");
    window->set_rect(200, 150, 300, 200);
    auto widget = window->set_main_widget<GUI::Widget>();
    widget->set_tooltip("SDE tooltip test"_string);
    window->show();

    // Request the tooltip after the window is up; Application shows it on its own
    // 700ms timer, so wait past that before quitting.
    auto request = Core::Timer::create_single_shot(400, [&] {
        app->show_tooltip("SDE tooltip test"_string, widget.ptr());
        dbgln("TOOLTIP-TEST: requested");
    });
    request->start();

    auto quit = Core::Timer::create_single_shot(1800, [&] { app->quit(0); });
    quit->start();
    return app->exec();
}
