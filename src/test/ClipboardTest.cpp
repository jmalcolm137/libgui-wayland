/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <AK/StringView.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGUI/Clipboard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>
#include <unistd.h>

// Reads the compositor clipboard, logs what it saw, then publishes something in
// the requested mode ("", "image", or "uri-list"). Used to exercise the native
// Wayland clipboard bridge end to end.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    // The portal learns the selection asynchronously on its own thread; give it a
    // moment before the initial snapshot so the test does not race the compositor.
    usleep(300 * 1000);

    auto before = GUI::Clipboard::the().fetch_data_and_type();
    dbgln("CLIPBOARD-TEST: read mime='{}' bytes={}", before.mime_type, before.data.size());
    for (auto const& entry : GUI::Clipboard::the().fetch_entries())
        dbgln("CLIPBOARD-TEST: entry mime='{}' bytes={}", entry.mime_type, entry.data.size());
    if (before.mime_type == "image/x-serenityos"sv) {
        auto bitmap = before.as_bitmap();
        dbgln("CLIPBOARD-TEST: decoded image {}x{}",
            bitmap ? bitmap->width() : -1,
            bitmap ? bitmap->height() : -1);
    } else if (!before.data.is_empty()) {
        dbgln("CLIPBOARD-TEST: content='{}'",
            StringView { reinterpret_cast<char const*>(before.data.data()), before.data.size() });
    }

    StringView mode = arguments.strings.size() > 1 ? arguments.strings[1] : ""sv;

    // "windowed" publishes after the window has been mapped and focused: a
    // compliant compositor only accepts a selection with a serial from a recent
    // input event, and this application has no other surface to receive one on.
    if (mode == "windowed"sv) {
        auto window = GUI::Window::construct();
        window->set_title("LibWM Clipboard Test");
        window->set_rect(100, 100, 200, 120);
        auto widget = window->set_main_widget<GUI::Widget>();
        widget->set_fill_with_background_color(true);
        window->show();
        auto publish = Core::Timer::create_single_shot(900, [&] {
            GUI::Clipboard::the().set_plain_text("libwm-clipboard-hello"sv);
            dbgln("CLIPBOARD-TEST: wrote text");
        });
        publish->start();
        auto quit = Core::Timer::create_single_shot(5000, [&] { app->quit(0); });
        quit->start();
        return app->exec();
    }

    if (mode == "image"sv) {
        auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 3, 3 }));
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                bitmap->set_pixel(x, y, Gfx::Color::from_rgb(0x00ff00));
        GUI::Clipboard::the().set_bitmap(*bitmap);
        dbgln("CLIPBOARD-TEST: wrote image");
    } else if (mode == "uri-list"sv) {
        GUI::Clipboard::the().set_data("file:///tmp/libwm-a\nfile:///tmp/libwm-b\n"sv.bytes(), "text/uri-list"sv);
        dbgln("CLIPBOARD-TEST: wrote uri-list");
    } else if (mode == "formats"sv) {
        auto append = [](Vector<GUI::ClipboardEntry>& entries, StringView contents, ByteString mime_type) {
            auto data = ByteBuffer::copy(contents.bytes());
            if (!data.is_error())
                entries.append({ data.release_value(), move(mime_type), {} });
        };

        Vector<GUI::ClipboardEntry> entries;
        append(entries, "libwm-plain-hello"sv, "text/plain");
        append(entries, "<b>libwm-rich-hello</b>"sv, "text/html");
        GUI::Clipboard::the().set_entries(move(entries));
        dbgln("CLIPBOARD-TEST: wrote formats");
    } else {
        GUI::Clipboard::the().set_plain_text("libwm-clipboard-hello"sv);
        dbgln("CLIPBOARD-TEST: wrote text");
    }

    auto quit_timer = Core::Timer::create_single_shot(5000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
