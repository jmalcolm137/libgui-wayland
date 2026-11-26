/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Exercises Serenity's set_global_cursor_position end to end: the pointer moves
// into the window (giving the shim a wl_pointer.enter serial), the widget asks
// the window server to put the cursor at a given output position, and the
// compositor applies the wp_pointer_warp_v1 request and reports a motion there.

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

class WarpWidget final : public GUI::Widget {
    C_OBJECT(WarpWidget)

public:
    Function<void()> on_warp_requested;

private:
    virtual void mousemove_event(GUI::MouseEvent& event) override
    {
        dbgln("WARP-TEST: mouse at {},{}", event.x(), event.y());
        if (m_requested || event.x() != 120 || event.y() != 90)
            return;
        m_requested = true;
        dbgln("WARP-TEST: requesting warp to 190,140");
        if (on_warp_requested)
            on_warp_requested();
    }

    bool m_requested { false };
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Warp Test");
    window->set_rect(100, 100, 400, 300);
    auto widget = window->set_main_widget<WarpWidget>();
    widget->set_fill_with_background_color(true);
    widget->on_warp_requested = [&] {
        // LibWM maps this output position onto the surface under it; the headless
        // compositor reports the resulting wl_pointer.motion at 190,140.
        GUI::ConnectionToWindowServer::the().async_set_global_cursor_position({ 190, 140 });
    };
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(2200, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
