/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// xdg-activation-v1: a window opened in response to a click must ask the
// compositor to raise it (a token requested with the click's serial), and a
// launcher-provided XDG_ACTIVATION_TOKEN must be used for the first window.
// The suite runs this app both ways and reads the compositor's activation log.

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

class ActivationWidget final : public GUI::Widget {
    C_OBJECT(ActivationWidget)

public:
    Function<void()> on_first_click;

private:
    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (m_clicked || event.button() != GUI::MouseButton::Primary)
            return;
        m_clicked = true;
        if (on_first_click)
            on_first_click();
    }

    bool m_clicked { false };
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Activation Test A");
    window->set_rect(100, 100, 320, 240);
    auto widget = window->set_main_widget<ActivationWidget>();
    widget->set_fill_with_background_color(true);
    window->show();
    dbgln("ACTIVATION-TEST: window A shown");

    RefPtr<GUI::Window> second_window;
    widget->on_first_click = [&] {
        second_window = GUI::Window::construct();
        second_window->set_title("Activation Test B");
        second_window->set_rect(180, 180, 240, 160);
        (void)second_window->set_main_widget<GUI::Widget>();
        second_window->show();
        dbgln("ACTIVATION-TEST: window B created");
    };

    auto quit_timer = Core::Timer::create_single_shot(2500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
