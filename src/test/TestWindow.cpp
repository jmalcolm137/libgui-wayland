/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include "WaylandClient.h"
#include "WaylandSeat.h"
#include <AK/StringBuilder.h>
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
#include <LibGfx/StandardCursor.h>
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
        if (window())
            window()->set_cursor(Gfx::StandardCursor::Hand);
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
    window->set_obey_widget_min_size(false);
    window->set_minimum_size(120, 90);
    window->set_cursor(Gfx::StandardCursor::Hand);

    auto widget = window->set_main_widget<ColorWidget>();
    widget->set_fill_with_background_color(true);
    window->show();
    window->set_modified(true);
    dbgln("TestWindow: modified={} min_size={}x{}", window->is_modified(), window->minimum_size().width(), window->minimum_size().height());

    // Exercise Wayland connection-lost reporting and graceful shutdown when the
    // headless compositor finishes and closes the Wayland socket.
    LibWM::WaylandClient::the().set_connection_lost_callback([&](ByteString const& reason) {
        dbgln("TestWindow: connection lost reason='{}'", reason);
        app->quit(0);
    });

    // Exercise zwp_text_input_v3 IME preedit and commit forwarding.
    LibWM::WaylandClient::the().seat().set_text_input_callbacks(
        [](String const& committed) {
            dbgln("TestWindow: IME commit='{}'", committed);
        },
        [](String const& preedit, i32 cursor_begin, i32 cursor_end) {
            dbgln("TestWindow: IME preedit='{}' cursor={}..{}", preedit, cursor_begin, cursor_end);
        },
        [](i32 before, i32 after) {
            dbgln("TestWindow: IME delete_surrounding={}..{}", before, after);
        });

    auto ime_and_title_timer = Core::Timer::create_single_shot(300, [&] {
        // Exercise UTF-8 clamping on an oversized (>4 KiB) Wayland window title so
        // xdg_toplevel.set_title never overflows libwayland's 4 KiB wire buffer.
        StringBuilder oversized;
        for (int i = 0; i < 600; ++i)
            oversized.append("🌊Wayland"sv);
        window->set_title(oversized.to_byte_string());
        dbgln("TestWindow: oversized UTF-8 title clamped and sent");

        LibWM::WaylandClient::the().seat().set_text_input_cursor_rect({ 10, 10, 2, 16 });
        LibWM::WaylandClient::the().seat().set_text_input_enabled(true);

        if (auto custom_cursor = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 12, 12 }); !custom_cursor.is_error())
            window->set_cursor(*custom_cursor.value());

        window->set_has_alpha_channel(true);
        window->set_alpha_hit_threshold(0.5f);
        widget->update();
    });
    ime_and_title_timer->start();

    // The shim owns the presented window state; the WindowServer connection
    // answers the client's queries from it. The full title survives (only the
    // copy sent to the compositor is clamped), and the alpha flag follows the
    // backing store.
    auto state_timer = Core::Timer::create_single_shot(1500, [&] {
        auto& wayland = LibWM::WaylandClient::the();
        auto const id = window->window_id();
        auto const min_size = wayland.window_minimum_size(id);
        dbgln("TestWindow: shim state title_bytes={} min_size={}x{} alpha={}",
            wayland.window_title(id).length(), min_size.width(), min_size.height(), wayland.window_has_alpha(id));
        window->set_title("LibWM Test Window");
    });
    state_timer->start();

    auto quit_timer = Core::Timer::create_single_shot(6000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
