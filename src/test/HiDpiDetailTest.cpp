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

// Minimal HiDPI fine-detail repro: a widget that draws full-width rectangles and
// full-width thin/thick lines. Rendered at 1x and 2x (SDE_OUTPUT_SCALE), then the
// window backing store is dumped (LIBWM_DUMP) and compared: any op that halves at
// 2x shows up as a line/rect that stops at the horizontal midpoint.
class DetailWidget final : public GUI::Widget {
    C_OBJECT(DetailWidget)

public:
    virtual ~DetailWidget() override = default;

private:
    virtual void paint_event(GUI::PaintEvent&) override
    {
        GUI::Painter painter(*this);
        auto w = width();
        auto h = height();

        // Background so extents are obvious.
        painter.fill_rect(rect(), Gfx::Color::from_rgb(0xffffff));

        // Full-width rectangles (control: these reportedly scale correctly).
        painter.fill_rect({ 0, 6, w, 8 }, Gfx::Color::from_rgb(0xff0000));
        painter.fill_rect({ 0, 16, w, 1 }, Gfx::Color::from_rgb(0x0000ff));

        // Full-width thin line (draw_line, thickness 1).
        painter.draw_line({ 0, 26 }, { w - 1, 26 }, Gfx::Color::from_rgb(0x008000));

        // Full-width thick line (draw_line, thickness 3).
        painter.draw_line({ 0, 36 }, { w - 1, 36 }, Gfx::Color::from_rgb(0xff00ff), 3);

        // Full-width rect outline (draw_rect) - bevel-style borders are drawn with this.
        painter.draw_rect({ 0, 46, w, 16 }, Gfx::Color::from_rgb(0x000000));

        // Full-width 'rough' rect outline (used for classic bevels).
        painter.draw_rect({ 0, 68, w, 16 }, Gfx::Color::from_rgb(0x404040), true);

        // Vertical line down the middle as a reference (should not be affected).
        painter.draw_line({ w / 2, 0 }, { w / 2, h - 1 }, Gfx::Color::from_rgb(0xffa500));
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("HiDPI Detail Test");
    window->set_rect(0, 0, 200, 96);

    auto widget = window->set_main_widget<DetailWidget>();
    widget->set_fill_with_background_color(true);
    window->show();

    auto quit_timer = Core::Timer::create_single_shot(6000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
