/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/TextAlignment.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title(ByteString { "Greeter" });
    window->resize(460, 300);

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_min_size(380, 260);
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 24 }, 10);

    auto& title = main_widget->add<GUI::Label>("Greeter"_string);
    title.set_font_size(24);
    title.set_font_weight(700);
    title.set_text_alignment(Gfx::TextAlignment::Center);

    auto& subtitle = main_widget->add<GUI::Label>("Type your name and press Greet."_string);
    subtitle.set_text_alignment(Gfx::TextAlignment::Center);
    subtitle.set_foreground_role(Gfx::ColorRole::DisabledText);

    // A nested horizontal layout: fixed-width label, growing text box, button.
    auto& name_row = main_widget->add<GUI::Widget>();
    name_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 8);

    auto& name_label = name_row.add<GUI::Label>("Name:"_string);
    name_label.set_fixed_width(48);

    auto& name_input = name_row.add<GUI::TextBox>();
    name_input.set_text("Ada"_string);

    auto& greet_button = name_row.add<GUI::Button>("Greet"_string);
    greet_button.set_fixed_width(84);

    auto& greeting = main_widget->add<GUI::Label>("Hello, Ada!"_string);
    greeting.set_font_size(20);
    greeting.set_text_alignment(Gfx::TextAlignment::Center);

    main_widget->add_spacer();

    auto& footer = main_widget->add<GUI::Widget>();
    footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 8);
    footer.add_spacer();

    auto& quit_button = footer.add<GUI::Button>("Quit"_string);
    quit_button.set_fixed_width(84);

    // The callback runs on the event loop, after the button has been released.
    // It captures by reference: all of these objects outlive the window.
    auto greet = [&] {
        auto name = name_input.text();
        if (name.is_empty())
            greeting.set_text("Hello, stranger!"_string);
        else
            greeting.set_text(String::formatted("Hello, {}!", name).release_value());
    };

    greet_button.on_click = [&](unsigned) {
        greet();
    };

    // Pressing Return in the text box does the same thing as clicking Greet.
    name_input.on_return_pressed = [&] {
        greet();
    };

    quit_button.on_click = [&](unsigned) {
        app->quit(0);
    };

    window->show();
    name_input.set_focus(true);
    return app->exec();
}
