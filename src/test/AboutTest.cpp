/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibCore/Version.h>
#include <LibGUI/AboutDialog.h>
#include <LibGUI/AboutDialogWidget.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("AboutTest");
    window->set_rect(200, 200, 250, 215);
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(4000, [&] { app->quit(0); });
    quit_timer->start();

    dbgln("ABOUT-TEST: showing AboutDialog");
    if (auto widget = GUI::AboutDialogWidget::try_create(); widget.is_error())
        dbgln("ABOUT-TEST: AboutDialogWidget::try_create error: {}", widget.error());
    else
        dbgln("ABOUT-TEST: AboutDialogWidget::try_create ok");
    GUI::AboutDialog::show("Calculator"_string, Core::Version::read_long_version_string().release_value_but_fixme_should_propagate_errors(), nullptr, window.ptr());
    dbgln("ABOUT-TEST: AboutDialog::show returned");

    return app->exec();
}
