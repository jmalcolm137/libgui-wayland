/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DateTime.h>
#include <LibCore/Timer.h>
#include <LibGUI/AboutDialog.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/TextAlignment.h>
#include <LibMain/Main.h>
#include <math.h>

namespace {

class ClockWidget final : public GUI::Widget {
    C_OBJECT(ClockWidget)

public:
    virtual ~ClockWidget() override = default;

    void set_show_seconds(bool show)
    {
        if (m_show_seconds == show)
            return;
        m_show_seconds = show;
        update();
    }

    bool show_seconds() const { return m_show_seconds; }

private:
    Gfx::IntPoint point_on_circle(Gfx::IntPoint center, double angle, double radius) const
    {
        return {
            center.x() + static_cast<int>(lround(cos(angle) * radius)),
            center.y() + static_cast<int>(lround(sin(angle) * radius)),
        };
    }

    void draw_hand(GUI::Painter& painter, Gfx::IntPoint center, double angle, double length, Gfx::Color color, int thickness)
    {
        painter.draw_line(center, point_on_circle(center, angle, length), color, thickness);
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);

        auto background = palette().color(Gfx::ColorRole::Window);
        auto face_color = palette().color(Gfx::ColorRole::Base);
        auto text_color = palette().color(Gfx::ColorRole::BaseText);
        auto ring_color = palette().color(Gfx::ColorRole::ActiveWindowBorder1);
        auto minute_color = palette().color(Gfx::ColorRole::ActiveWindowBorder2);
        auto hour_color = text_color;
        auto second_color = Gfx::Color::from_rgb(0xb03a2e);

        painter.fill_rect(event.rect(), background);

        auto center = rect().center();
        auto radius = min(width(), height()) / 2 - 48;
        if (radius <= 0)
            return;

        Gfx::IntRect face {
            center.x() - radius,
            center.y() - radius,
            radius * 2,
            radius * 2,
        };
        painter.fill_ellipse(face, face_color);
        painter.draw_ellipse_intersecting(face, ring_color, 2);

        // 60 minute ticks; every fifth one is longer and thicker.
        for (int i = 0; i < 60; ++i) {
            auto angle = (i * 2.0 * M_PI / 60.0) - M_PI_2;
            auto outer = point_on_circle(center, angle, radius - 8);
            auto inner = point_on_circle(center, angle, radius - (i % 5 == 0 ? 20 : 14));
            painter.draw_line(inner, outer, text_color, i % 5 == 0 ? 2 : 1);
        }

        auto now = Core::DateTime::now();

        auto hour_angle = ((now.hour() % 12) + now.minute() / 60.0) * 2.0 * M_PI / 12.0 - M_PI_2;
        auto minute_angle = (now.minute() + now.second() / 60.0) * 2.0 * M_PI / 60.0 - M_PI_2;
        auto second_angle = now.second() * 2.0 * M_PI / 60.0 - M_PI_2;

        draw_hand(painter, center, hour_angle, radius * 0.50, hour_color, 6);
        draw_hand(painter, center, minute_angle, radius * 0.72, minute_color, 3);
        if (m_show_seconds)
            draw_hand(painter, center, second_angle, radius * 0.84, second_color, 1);

        painter.fill_ellipse({ center.x() - 5, center.y() - 5, 10, 10 }, ring_color);

        auto digital = now.to_byte_string(m_show_seconds ? "%H:%M:%S"sv : "%H:%M"sv);
        auto date = now.to_byte_string("%A, %d %B %Y"sv);
        Gfx::IntRect digital_rect { center.x() - radius, center.y() + radius / 2, radius * 2, 24 };
        Gfx::IntRect date_rect { center.x() - radius, center.y() + radius + 10, radius * 2, 20 };
        painter.draw_text(
            digital_rect,
            digital.view(),
            Gfx::FontDatabase::default_font().bold_variant(),
            Gfx::TextAlignment::Center,
            text_color);
        painter.draw_text(
            date_rect,
            date.view(),
            Gfx::TextAlignment::Center,
            palette().color(Gfx::ColorRole::DisabledText));
    }

    bool m_show_seconds { true };
};

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto app_icon = TRY(GUI::Icon::try_create_default_icon("app-analog-clock"sv));

    auto window = GUI::Window::construct();
    window->set_title(ByteString { "Sundial" });
    window->set_icon(app_icon.bitmap_for_size(16));
    window->resize(380, 420);

    auto clock = window->set_main_widget<ClockWidget>();
    clock->set_min_size(260, 300);

    auto show_seconds_action = GUI::Action::create_checkable(
        ByteString { "Show &Seconds" },
        { Mod_Ctrl, Key_S },
        [&](auto& action) {
            clock->set_show_seconds(action.is_checked());
        });
    show_seconds_action->set_checked(true);

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::CommonActions::make_quit_action([&](auto&) {
        app->quit(0);
    }));

    auto view_menu = window->add_menu("&View"_string);
    view_menu->add_action(*show_seconds_action);

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::CommonActions::make_about_action("Sundial"_string, app_icon, window));

    auto timer = Core::Timer::create_repeating(1000, [&] {
        clock->update();
    });
    timer->start();

    window->show();
    return app->exec();
}
