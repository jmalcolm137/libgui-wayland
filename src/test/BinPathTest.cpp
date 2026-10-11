/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibCore/Process.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

// Verifies the session /bin shadow (SDE R4.2): Process::resolve_executable_path()
// consults SDE_SESSION_BIN_DIR, so an unmodified SerenityOS app that spawns
// /bin/shutdown (the Taskbar "Exit..." menu's ShutdownDialog) reaches the
// session's sde-power helper. Needs no compositor.
ErrorOr<int> serenity_main(Main::Arguments)
{
    char tmpl[] = "/tmp/sde-binpath-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        warnln("BINPATH-TEST: mkdtemp failed");
        return 1;
    }

    ByteString helper = ByteString::formatted("{}/shutdown", dir);
    int fd = ::open(helper.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        warnln("BINPATH-TEST: could not create helper");
        return 1;
    }
    (void)::write(fd, "#!/bin/sh\nexit 0\n", 17);
    ::close(fd);

    setenv("SDE_SESSION_BIN_DIR", dir, 1);

    auto resolved = Core::Process::resolve_executable_path("/bin/shutdown"sv);
    outln("BINPATH-TEST: /bin/shutdown -> {}", resolved);
    bool shadowed = resolved == helper;
    outln("BINPATH-TEST: shadowed by SDE_SESSION_BIN_DIR={}", shadowed);

    // A path with no session helper falls through to the normal mapping.
    auto settings = Core::Process::resolve_executable_path("/bin/Settings"sv);
    outln("BINPATH-TEST: /bin/Settings -> {}", settings);
    bool falls_through = !settings.is_empty() && settings != ByteString::formatted("{}/Settings", dir);
    outln("BINPATH-TEST: falls through={}", falls_through);

    ::unlink(helper.characters());
    ::rmdir(dir);

    bool const ok = shadowed && falls_through;
    outln("BINPATH-TEST: ok={}", ok);
    return ok ? 0 : 1;
}
