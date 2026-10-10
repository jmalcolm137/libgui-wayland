/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibDesktop/AppFile.h>
#include <LibMain/Main.h>

// Spawns an application through Desktop::AppFile - the exact path the Taskbar's
// system-menu entries use - so the launcher wiring can be exercised. Run it
// inside a live session and confirm the app's window appears. See
// docs/07-systemserver-roles.md.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    if (arguments.strings.size() < 2)
        return Error::from_string_literal("usage: libwm-appfile-test <AppName> [args...]");

    auto name = arguments.strings[1];
    auto app_file = Desktop::AppFile::get_for_app(name);
    if (!app_file->is_valid())
        return Error::from_string_literal("AppFile is invalid (missing or malformed)");

    Vector<StringView> extra_arguments;
    for (size_t i = 2; i < arguments.strings.size(); ++i)
        extra_arguments.append(arguments.strings[i]);

    TRY(app_file->spawn_with_escalation(extra_arguments.span()));
    dbgln("APPFILE-LAUNCH: spawned '{}' ({})", name, app_file->executable());
    return 0;
}
