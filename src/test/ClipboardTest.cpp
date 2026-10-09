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
#include <LibMain/Main.h>

// Reads the compositor clipboard, then publishes our own text, then exits.
// Used to exercise the native Wayland clipboard bridge end to end.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    auto before = GUI::Clipboard::the().fetch_data_and_type();
    dbgln("CLIPBOARD-TEST: read mime='{}' data='{}'",
        before.mime_type,
        StringView { reinterpret_cast<char const*>(before.data.data()), before.data.size() });

    GUI::Clipboard::the().set_plain_text("libwm-clipboard-hello"sv);
    dbgln("CLIPBOARD-TEST: wrote 'libwm-clipboard-hello'");

    auto quit_timer = Core::Timer::create_single_shot(4000, [&] { app->quit(0); });
    quit_timer->start();
    return app->exec();
}
