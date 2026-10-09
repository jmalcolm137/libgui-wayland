#pragma once

// libgui-wayland host compat: SerenityOS's <serenity.h> declares Serenity-only
// syscalls. On the host, applications that include it really just use ordinary
// POSIX, so provide the standard headers here plus no-op stubs for the handful
// of syscalls they reference.
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static inline int disown(pid_t) { return 0; }

static inline int setkeymap(char const*, uint32_t const*, uint32_t* const, uint32_t const*, uint32_t const*, uint32_t const*)
{
    return -1;
}

static inline int getkeymap(char*, size_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*)
{
    return -1;
}
