/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

class FillWidget final : public GUI::Widget {
    C_OBJECT(FillWidget)

private:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.fill_rect(event.rect(), Gfx::Color::from_rgb(0x205020));
        painter.draw_rect(rect(), Gfx::Color::from_rgb(0xffffff));
    }

    virtual void resize_event(GUI::ResizeEvent& event) override
    {
        dbgln("FULLSCREEN-TEST: resized to {}x{}", event.size().width(), event.size().height());
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Fullscreen Test");
    window->set_rect(100, 100, 400, 300);
    auto widget = window->set_main_widget<FillWidget>();
    widget->set_fill_with_background_color(true);
    window->show();

    auto fullscreen_timer = Core::Timer::create_single_shot(600, [&] {
        dbgln("FULLSCREEN-TEST: set_fullscreen(true)");
        window->set_fullscreen(true);
        dbgln("FULLSCREEN-TEST: set_maximized(true)");
        window->set_maximized(true);
        dbgln("FULLSCREEN-TEST: set_maximized(false)");
        window->set_maximized(false);
        dbgln("FULLSCREEN-TEST: set_minimized(true)");
        window->set_minimized(true);
        dbgln("FULLSCREEN-TEST: set_fullscreen(false)");
        window->set_fullscreen(false);
    });
    fullscreen_timer->start();

    auto quit_timer = Core::Timer::create_single_shot(2500, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
