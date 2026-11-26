/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal org.freedesktop.portal.Settings service for tests.
//
// Exposes org.freedesktop.appearance color-scheme on the session bus, and can
// change it after a delay (emitting SettingChanged) to exercise a client that
// follows appearance updates. For tests only.
//
//   fake-settings-portal [--scheme 0|1|2] [--change-after MS --change-to N]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

static uint32_t g_scheme = 1;
static uint32_t g_change_after_ms = 0;
static uint32_t g_change_to = 1;

static int method_read(sd_bus_message* message, void*, sd_bus_error*)
{
    char const* name_space = nullptr;
    char const* key = nullptr;
    if (sd_bus_message_read(message, "ss", &name_space, &key) < 0)
        return -EINVAL;
    if (!name_space || !key || strcmp(name_space, "org.freedesktop.appearance") || strcmp(key, "color-scheme"))
        return sd_bus_error_set(nullptr, "org.freedesktop.portal.Error.NotFound", "no such setting");

    sd_bus_message* reply = nullptr;
    if (sd_bus_message_new_method_return(message, &reply) < 0)
        return -ENOMEM;
    sd_bus_message_append(reply, "v", "u", g_scheme);
    int const result = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return result < 0 ? result : 1;
}

static sd_bus_vtable const settings_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scheme") && i + 1 < argc)
            g_scheme = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--change-after") && i + 1 < argc)
            g_change_after_ms = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--change-to") && i + 1 < argc)
            g_change_to = (uint32_t)strtoul(argv[++i], nullptr, 10);
    }

    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) {
        fprintf(stderr, "fake-settings-portal: no session bus\n");
        return 1;
    }
    if (sd_bus_add_object_vtable(bus, nullptr, "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", settings_vtable, nullptr) < 0) {
        fprintf(stderr, "fake-settings-portal: cannot export the settings object\n");
        return 1;
    }
    if (sd_bus_request_name(bus, "org.freedesktop.portal.Desktop", 0) < 0) {
        fprintf(stderr, "fake-settings-portal: cannot own the portal name\n");
        return 1;
    }
    printf("fake-settings-portal: serving color-scheme=%u\n", g_scheme);
    fflush(stdout);

    uint64_t const start = now_ms();
    bool changed = false;
    for (;;) {
        for (;;) {
            int const result = sd_bus_process(bus, nullptr);
            if (result <= 0)
                break;
        }
        if (g_change_after_ms > 0 && !changed && now_ms() - start >= g_change_after_ms) {
            changed = true;
            g_scheme = g_change_to;
            sd_bus_emit_signal(bus, "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.Settings", "SettingChanged",
                "ssv", "org.freedesktop.appearance", "color-scheme", "u", g_scheme);
            printf("fake-settings-portal: changed to color-scheme=%u\n", g_scheme);
            fflush(stdout);
        }
        (void)sd_bus_wait(bus, 20 * 1000);
    }
}
