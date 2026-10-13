/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/DragOperation.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

// A minimal LibGUI drag source: a mouse press starts a DragOperation with text,
// exactly as a Serenity app (e.g. a view) does. It reports the outcome, so
// scripts/run-libwm-drag-test.sh can assert the shim bridges LibGUI's drag onto
// the standard Wayland data-device (and back).
class DragWidget final : public GUI::Widget {
    C_OBJECT(DragWidget)

public:
    virtual ~DragWidget() override = default;

private:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.fill_rect(event.rect(), Gfx::Color::from_rgb(0x205020));
        painter.draw_rect(rect(), Gfx::Color::from_rgb(0xf0f0f0));
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() != GUI::MouseButton::Primary)
            return;
        dbgln("DRAG-TEST: press at {},{}", event.position().x(), event.position().y());

        auto drag = GUI::DragOperation::construct();
        drag->set_text("libwm-drag-hello");
        auto outcome = drag->exec();

        outln("DRAG-TEST: outcome={}", outcome == GUI::DragOperation::Outcome::Accepted ? "accepted" : "cancelled");
        GUI::Application::the()->quit(0);
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("LibWM Drag Test");
    window->set_rect(100, 100, 320, 240);

    auto widget = window->set_main_widget<DragWidget>();
    widget->set_fill_with_background_color(true);
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(8000, [&] { app->quit(1); });
    quit_timer->start();
    return app->exec();
}
