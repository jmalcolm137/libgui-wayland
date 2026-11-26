/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/EventLoop.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/Socket.h>
#include <LibDesktop/AppFile.h>
#include <LibDesktop/Launcher.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>

// Spawns an application through Desktop::AppFile - the exact path the Taskbar's
// system-menu entries use - or through Desktop::Launcher::open(--launcher), or
// exercises LibWM's Linux portal deferral and SOCKET_TAKEOVER service spawning
// (--test-portals).
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    if (arguments.strings.size() < 2)
        return Error::from_string_literal("usage: libwm-appfile-test <AppName|--test-portals> [--launcher] [args...]");

    if (arguments.strings[1] == "--test-portals"sv) {
        auto notify_path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/notify"sv));
        (void)Core::LocalSocket::connect(notify_path);

        auto request_path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/request"sv));
        auto req_sock = Core::LocalSocket::connect(request_path);
        dbgln("PORTAL-TEST: request connect ok={}", !req_sock.is_error());

        auto sql_path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/sql"sv));
        auto sql_sock = Core::LocalSocket::connect(sql_path);
        dbgln("PORTAL-TEST: sql connect ok={}", !sql_sock.is_error());
        return 0;
    }

    // A name is looked up in the resource root's apps/ directory; a path (used
    // by the suite, since the suite's launch target is not a Serenity app) is
    // opened directly.
    auto name = arguments.strings[1];
    auto app_file = name.ends_with(".af"sv) ? Desktop::AppFile::open(name) : Desktop::AppFile::get_for_app(name);
    if (!app_file->is_valid())
        return Error::from_string_literal("AppFile is invalid (missing or malformed)");

    if (arguments.strings.size() >= 3 && arguments.strings[2] == "--launcher"sv) {
        Core::EventLoop loop;
        auto url = URL::create_with_file_scheme(app_file->executable());
        bool opened = Desktop::Launcher::open(url);
        if (!opened)
            return Error::from_string_literal("Desktop::Launcher::open failed");
        dbgln("LAUNCHER-OPEN: opened '{}' ({})", app_file->name(), app_file->executable());
        return 0;
    }

    Vector<StringView> extra_arguments;
    for (size_t i = 2; i < arguments.strings.size(); ++i)
        extra_arguments.append(arguments.strings[i]);

    TRY(app_file->spawn_with_escalation(extra_arguments.span()));
    dbgln("APPFILE-LAUNCH: spawned '{}' ({})", app_file->name(), app_file->executable());
    return 0;
}
