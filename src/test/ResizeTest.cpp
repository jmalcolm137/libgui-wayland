/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Interactive resize: a window with a statusbar (so its ResizeCorner exists)
// asks for a bottom-right resize the way the corner does, and then for a
// direction that must not produce a request. The compositor drives the resize
// (xdg_toplevel.resize), so the app-side effect is that LibWM sends the request.

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM Resize Test");
    window->set_rect(100, 100, 400, 300);

    auto widget = window->set_main_widget<GUI::Widget>();
    widget->set_fill_with_background_color(true);
    widget->set_layout<GUI::VerticalBoxLayout>();
    auto& statusbar = widget->add<GUI::Statusbar>();
    statusbar.set_text(MUST(String::from_utf8("resize test"sv)));
    window->show();

    // The compositor's script has clicked inside the window by now, so a seat
    // serial exists: xdg_toplevel.resize requires the serial of the press that
    // asked for the resize.
    auto resize_timer = Core::Timer::create_single_shot(800, [&] {
        window->start_interactive_resize(GUI::ResizeDirection::DownRight);
        dbgln("ResizeTest: asked for a bottom-right resize");
    });
    resize_timer->start();

    auto no_resize_timer = Core::Timer::create_single_shot(1200, [&] {
        window->start_interactive_resize(GUI::ResizeDirection::None);
        dbgln("ResizeTest: asked for no resize");
    });
    no_resize_timer->start();

    auto quit_timer = Core::Timer::create_single_shot(2500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
