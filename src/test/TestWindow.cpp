/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/MimeData.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/DragOperation.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

class ColorWidget final : public GUI::Widget {
    C_OBJECT(ColorWidget)

public:
    ColorWidget()
    {
        set_focus_policy(GUI::FocusPolicy::StrongFocus);
    }
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
        if (event.button() == GUI::MouseButton::Secondary) {
            auto drag = GUI::DragOperation::construct();
            drag->set_text("libwm-outgoing-drag"sv);
            if (auto icon = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 4, 4 }); !icon.is_error())
                drag->set_bitmap(icon.value().ptr());
            auto outcome = drag->exec();
            dbgln("TestWindow: outgoing drag finished outcome={}", (int)outcome);
        }
    }

    virtual void mouseup_event(GUI::MouseEvent& event) override
    {
        dbgln("TestWindow: mouse up at {},{} button {}", event.position().x(), event.position().y(), (int)event.button());
    }

    virtual void doubleclick_event(GUI::MouseEvent& event) override
    {
        dbgln("TestWindow: mouse double click at {},{} button {}", event.position().x(), event.position().y(), (int)event.button());
    }

    virtual void mousewheel_event(GUI::MouseEvent& event) override
    {
        dbgln("TestWindow: mouse wheel delta={},{} at {},{}", event.wheel_delta_x(), event.wheel_delta_y(), event.position().x(), event.position().y());
    }

    virtual void keydown_event(GUI::KeyEvent& event) override
    {
        dbgln("TestWindow: key down code_point={} key={} modifiers={}", event.code_point(), (int)event.key(), event.modifiers());
    }

    virtual void keyup_event(GUI::KeyEvent& event) override
    {
        dbgln("TestWindow: key up code_point={} key={} modifiers={}", event.code_point(), (int)event.key(), event.modifiers());
    }

    virtual void drag_enter_event(GUI::DragEvent& event) override
    {
        dbgln("TestWindow: drag enter at {},{}", event.position().x(), event.position().y());
        event.accept();
    }

    virtual void drag_move_event(GUI::DragEvent& event) override
    {
        dbgln("TestWindow: drag move at {},{}", event.position().x(), event.position().y());
        event.accept();
    }

    virtual void drop_event(GUI::DropEvent& event) override
    {
        ByteString payload = event.text();
        if (payload.is_empty() && event.mime_data().has_format("text/plain"sv)) {
            auto bytes = event.mime_data().data("text/plain"sv);
            payload = ByteString { ReadonlyBytes { bytes.data(), bytes.size() } };
        }
        dbgln("TestWindow: drop at {},{} text='{}'", event.position().x(), event.position().y(), payload);
        event.accept();
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
