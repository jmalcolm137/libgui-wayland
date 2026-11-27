/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal org.freedesktop.portal.Settings/Screenshot service for tests.
//
// Exposes org.freedesktop.appearance color-scheme on the session bus, and can
// change it after a delay (emitting SettingChanged) to exercise a client that
// follows appearance updates. It also answers Screenshot with a file URI after a
// short delay, so LibWM's portal fallback can be tested. For tests only.
//
//   fake-settings-portal [--scheme 0|1|2] [--change-after MS --change-to N]
//                        [--screenshot PATH]   (serves a copy of PATH as a PNG)

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

static uint32_t g_scheme = 1;
static uint32_t g_change_after_ms = 0;
static uint32_t g_change_to = 1;
static char g_screenshot_template[1024];
static bool g_screenshot_pending = false;
static uint64_t g_screenshot_respond_at = 0;
static char g_screenshot_handle[256];
static unsigned g_screenshot_serial = 0;

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

static uint64_t now_ms();

static int method_screenshot(sd_bus_message* message, void*, sd_bus_error*)
{
    char const* parent = nullptr;
    char token[128] = "token";
    if (sd_bus_message_read(message, "s", &parent) < 0)
        return -EINVAL;
    if (sd_bus_message_enter_container(message, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
            char const* key = nullptr;
            sd_bus_message_read(message, "s", &key);
            if (key && !strcmp(key, "handle_token")) {
                char const* value = nullptr;
                if (sd_bus_message_read(message, "v", "s", &value) > 0 && value)
                    snprintf(token, sizeof token, "%s", value);
            } else {
                sd_bus_message_skip(message, "v");
            }
            sd_bus_message_exit_container(message);
        }
        sd_bus_message_exit_container(message);
    }

    for (char* p = token; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            *p = '_';
    }
    snprintf(g_screenshot_handle, sizeof g_screenshot_handle,
        "/org/freedesktop/portal/desktop/request/fake/%s", token);
    g_screenshot_pending = true;
    g_screenshot_respond_at = now_ms() + 100;

    sd_bus_message* reply = nullptr;
    if (sd_bus_message_new_method_return(message, &reply) < 0)
        return -ENOMEM;
    sd_bus_message_append(reply, "o", g_screenshot_handle);
    int const result = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return result < 0 ? result : 1;
}

static sd_bus_vtable const settings_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Read", "ss", "v", method_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static sd_bus_vtable const screenshot_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Screenshot", "sa{sv}", "o", method_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
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
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            snprintf(g_screenshot_template, sizeof g_screenshot_template, "%s", argv[++i]);
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
    if (g_screenshot_template[0] && sd_bus_add_object_vtable(bus, nullptr, "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Screenshot", screenshot_vtable, nullptr) < 0) {
        fprintf(stderr, "fake-settings-portal: cannot export the screenshot object\n");
        return 1;
    }
    if (sd_bus_request_name(bus, "org.freedesktop.portal.Desktop", 0) < 0) {
        fprintf(stderr, "fake-settings-portal: cannot own the portal name\n");
        return 1;
    }
    printf("fake-settings-portal: serving color-scheme=%u%s%s\n", g_scheme,
        g_screenshot_template[0] ? ", screenshot " : "", g_screenshot_template);
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
        if (g_screenshot_pending && now_ms() >= g_screenshot_respond_at) {
            g_screenshot_pending = false;
            // The client unlinks the file it reads, so hand out a fresh copy.
            char copy[1100];
            snprintf(copy, sizeof copy, "%s.%u.png", g_screenshot_template, ++g_screenshot_serial);
            bool copied = false;
            FILE* in = fopen(g_screenshot_template, "rb");
            FILE* out = in ? fopen(copy, "wb") : nullptr;
            if (out) {
                char buffer[4096];
                size_t n;
                while ((n = fread(buffer, 1, sizeof buffer, in)) > 0)
                    fwrite(buffer, 1, n, out);
                copied = true;
            }
            if (in)
                fclose(in);
            if (out)
                fclose(out);
            if (!copied) {
                fprintf(stderr, "fake-settings-portal: cannot copy %s\n", g_screenshot_template);
                continue;
            }
            sd_bus_message* signal = nullptr;
            if (sd_bus_message_new_signal(bus, &signal, g_screenshot_handle,
                    "org.freedesktop.portal.Request", "Response") >= 0) {
                char uri[1200];
                snprintf(uri, sizeof uri, "file://%s", copy);
                sd_bus_message_append(signal, "u", 0);
                sd_bus_message_open_container(signal, 'a', "{sv}");
                sd_bus_message_open_container(signal, 'e', "sv");
                sd_bus_message_append(signal, "s", "uri");
                sd_bus_message_open_container(signal, 'v', "s");
                sd_bus_message_append(signal, "s", uri);
                sd_bus_message_close_container(signal);
                sd_bus_message_close_container(signal);
                sd_bus_message_close_container(signal);
                sd_bus_send(bus, signal, nullptr);
                sd_bus_message_unref(signal);
                printf("fake-settings-portal: screenshot response %s\n", uri);
                fflush(stdout);
            }
        }
        (void)sd_bus_wait(bus, 20 * 1000);
    }
}
