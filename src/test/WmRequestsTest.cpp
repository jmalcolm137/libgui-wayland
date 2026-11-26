/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Exercises the WindowServer requests that Wayland cannot express (or that map
// only partly), so the suite can check that each one is handled explicitly:
// base size/increment, aspect ratio, forced shadow, flash flush, global mouse
// tracking, screen numbers and a theme refresh.

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibGUI/MouseTracker.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

class NullMouseTracker final : public GUI::MouseTracker {
protected:
    virtual void track_mouse_move(Gfx::IntPoint) override { }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Wm Requests Test");
    window->set_rect(100, 100, 400, 300);
    (void)window->set_main_widget<GUI::Widget>();
    window->show();

    dbgln("WMREQ-TEST: sending requests");

    // base size becomes the window's minimum size; the increment has no
    // xdg-toplevel equivalent.
    window->set_size_increment({ 10, 10 });
    window->set_base_size({ 250, 150 });
    window->set_resize_aspect_ratio(4, 3);
    window->set_forced_shadow(true);

    // Reload the theme in effect and tell the desktop to overlay output numbers.
    window->refresh_system_theme();
    GUI::ConnectionToWindowServer::the().async_show_screen_numbers(true);
    GUI::ConnectionToWindowServer::the().async_set_flash_flush(true);

    // Constructing a MouseTracker asks for global mouse tracking; constructing
    // and destroying it also proves the disable path does not hang.
    {
        NullMouseTracker tracker;
        dbgln("WMREQ-TEST: mouse tracker active");
    }
    dbgln("WMREQ-TEST: mouse tracker released");

    // Leave time for the asynchronous requests to arrive.
    auto quit_timer = Core::Timer::create_single_shot(1200, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
