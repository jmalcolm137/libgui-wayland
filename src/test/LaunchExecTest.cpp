/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/EventLoop.h>
#include <LibDesktop/Launcher.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>

// Opens an *executable* file URL through Desktop::Launcher - the path the
// desktop's right-click "Display Settings" entry uses. The LaunchServer must run
// the executable (mapping Serenity's "/bin/X" to the host session app dir),
// not resolve it as a document. See docs/07-systemserver-roles.md.
ErrorOr<int> serenity_main(Main::Arguments)
{
    LibWM::initialize();
    // The launch portal connection dispatches synchronously through the current
    // event loop (as in every GUI app), so create one before opening the URL.
    Core::EventLoop loop;

    bool ok = Desktop::Launcher::open(URL::create_with_file_scheme("/bin/DisplaySettings"sv));
    dbgln("LAUNCH-EXEC-TEST: opened /bin/DisplaySettings = {}", ok);
    return ok ? 0 : 1;
}
