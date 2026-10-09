/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <AK/StringView.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Clipboard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibMain/Main.h>

// Reads the compositor clipboard, logs what it saw, then publishes something in
// the requested mode ("", "image", or "uri-list"). Used to exercise the native
// Wayland clipboard bridge end to end.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto before = GUI::Clipboard::the().fetch_data_and_type();
    dbgln("CLIPBOARD-TEST: read mime='{}' bytes={}", before.mime_type, before.data.size());
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
    } else {
        GUI::Clipboard::the().set_plain_text("libwm-clipboard-hello"sv);
        dbgln("CLIPBOARD-TEST: wrote text");
    }

    auto quit_timer = Core::Timer::create_single_shot(5000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
