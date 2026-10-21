/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    // Every LibGUI program starts here. Application owns the connection to the
    // display, the event loop, and the application-wide theme.
    auto app = TRY(GUI::Application::create(arguments));

    // A Window is the toplevel: on Wayland it becomes an xdg_toplevel and the
    // compositor is free to place it wherever it likes.
    auto window = GUI::Window::construct();
    window->set_title(ByteString { "Hello, Wayland!" });
    window->resize(360, 200);

    // A Window has exactly one main widget. Every other widget is its child.
    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 24 }, 12);

    auto& label = main_widget->add<GUI::Label>("Hello, Wayland!"_string);
    label.set_font_size(20);
    label.set_font_weight(700);
    label.set_text_alignment(Gfx::TextAlignment::Center);

    auto& hint = main_widget->add<GUI::Label>("A LibGUI application, running as a native Wayland client."_string);
    hint.set_text_alignment(Gfx::TextAlignment::Center);

    main_widget->add_spacer();

    auto& quit_button = main_widget->add<GUI::Button>("Quit"_string);
    quit_button.set_fixed_height(28);
    quit_button.on_click = [&](unsigned) {
        app->quit(0);
    };

    window->show();
    return app->exec();
}
