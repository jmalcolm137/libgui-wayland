/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

class ColorWidget final : public GUI::Widget {
    C_OBJECT(ColorWidget)

public:
    virtual ~ColorWidget() override = default;

private:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.fill_rect(event.rect(), Gfx::Color::from_rgb(0x3050a0));
        painter.draw_rect(rect(), Gfx::Color::from_rgb(0xf0f0f0));
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        dbgln("TestWindow: mouse down at {},{} button {}", event.position().x(), event.position().y(), (int)event.button());
    }

    virtual void keydown_event(GUI::KeyEvent& event) override
    {
        dbgln("TestWindow: key down code_point={} key={} modifiers={}", event.code_point(), (int)event.key(), event.modifiers());
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM Test Window");
    window->set_rect(100, 100, 320, 240);

    auto widget = window->set_main_widget<ColorWidget>();
    widget->set_fill_with_background_color(true);
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(6000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
