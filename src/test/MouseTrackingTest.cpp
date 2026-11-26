/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Serenity's global mouse tracking: a GUI::MouseTracker must receive the
// pointer's output position while the pointer is outside the window too. The
// shim implements it with ext-input-observation-v1; the input script moves the
// pointer inside the window, then well outside it, then again after the tracker
// is destroyed (which must stop the events).

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/MouseTracker.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

class TrackingMouse final : public GUI::MouseTracker {
protected:
    virtual void track_mouse_move(Gfx::IntPoint point) override
    {
        dbgln("MOUSETRACK: {},{}", point.x(), point.y());
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Mouse Tracking Test");
    window->set_rect(100, 100, 320, 240);
    (void)window->set_main_widget<GUI::Widget>();
    window->show();
    dbgln("MOUSE-TRACK-TEST: window at {},{}", window->position().x(), window->position().y());

    // Constructed after the window exists: the observer must be created for it.
    OwnPtr<TrackingMouse> tracker = make<TrackingMouse>();

    auto disable_timer = Core::Timer::create_single_shot(1800, [&] {
        dbgln("MOUSE-TRACK-TEST: disabling");
        tracker.clear();
    });
    disable_timer->start();

    auto quit_timer = Core::Timer::create_single_shot(2800, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
