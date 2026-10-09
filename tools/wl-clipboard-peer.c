/*
 * wl-clipboard-peer.c — a tiny native Wayland clipboard client for tests.
 *
 * Built directly on libwayland-client (no X11, no GTK). Used to verify LibWM's
 * clipboard bridge in both directions against the headless compositor:
 *
 *   wl-clipboard-peer set "some text"   # own the selection, serve it, stay alive
 *   wl-clipboard-peer get               # print the selection's text, exit
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_display *display;
static struct wl_data_device_manager *manager;
static struct wl_seat *seat;
static struct wl_data_device *device;
static struct wl_data_source *source;
static struct wl_data_offer *current_offer;
static int got_selection;
static char const *set_text;
static char mimes[32][128];
static int mime_count;

static char const *preferred(void)
{
    char const *wanted[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING" };
    for (size_t w = 0; w < sizeof(wanted) / sizeof(wanted[0]); ++w)
        for (int i = 0; i < mime_count; ++i)
            if (!strcmp(mimes[i], wanted[w]))
                return wanted[w];
    return mime_count ? mimes[0] : NULL;
}

static void source_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd)
{
    (void)data; (void)src; (void)mime;
    if (set_text)
        (void)write(fd, set_text, strlen(set_text));
    close(fd);
}
static void source_cancelled(void *data, struct wl_data_source *src)
{
    (void)data;
    wl_data_source_destroy(src);
    source = NULL;
}
static void source_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void source_dnd_drop_performed(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void source_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void source_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }

static struct wl_data_source_listener const source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
    (void)data; (void)offer;
    if (mime_count < (int)(sizeof(mimes) / sizeof(mimes[0]))) {
        strncpy(mimes[mime_count], mime, sizeof(mimes[0]) - 1);
        mime_count++;
    }
}
static void offer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }

static struct wl_data_offer_listener const offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
    (void)data; (void)dev;
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
}
static void device_enter(void *d, struct wl_data_device *v, uint32_t a, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { (void)d;(void)v;(void)a;(void)s;(void)x;(void)y;(void)o; }
static void device_leave(void *d, struct wl_data_device *v) { (void)d;(void)v; }
static void device_motion(void *d, struct wl_data_device *v, uint32_t a, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)v;(void)a;(void)x;(void)y; }
static void device_drop(void *d, struct wl_data_device *v) { (void)d;(void)v; }
static void device_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
    (void)data; (void)dev;
    current_offer = offer;
    got_selection = 1;
}

static struct wl_data_device_listener const device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (!strcmp(interface, wl_data_device_manager_interface.name))
        manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    else if (!strcmp(interface, wl_seat_interface.name))
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s set TEXT | get\n", argv[0]);
        return 2;
    }
    int mode_set = !strcmp(argv[1], "set");
    if (mode_set) {
        if (argc < 3) { fprintf(stderr, "set needs TEXT\n"); return 2; }
        set_text = argv[2];
    }

    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "no compositor\n"); return 1; }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !seat) { fprintf(stderr, "missing globals\n"); return 1; }

    device = wl_data_device_manager_get_data_device(manager, seat);
    wl_data_device_add_listener(device, &device_listener, NULL);
    wl_display_roundtrip(display);

    if (mode_set) {
        source = wl_data_device_manager_create_data_source(manager);
        wl_data_source_add_listener(source, &source_listener, NULL);
        wl_data_source_offer(source, "text/plain;charset=utf-8");
        wl_data_source_offer(source, "text/plain");
        wl_data_source_offer(source, "UTF8_STRING");
        wl_data_device_set_selection(device, source, 0);
        wl_display_flush(display);
        printf("PEER: serving selection\n");
        fflush(stdout);
        for (;;) {
            if (wl_display_dispatch(display) < 0)
                break;
        }
        return 0;
    }

    for (int i = 0; i < 200 && !got_selection; ++i) {
        if (wl_display_roundtrip(display) < 0)
            break;
    }
    if (!got_selection || !current_offer) {
        fprintf(stderr, "PEER: no selection\n");
        return 1;
    }
    char const *mime = preferred();
    if (!mime) { fprintf(stderr, "PEER: no usable mime\n"); return 1; }

    int fds[2];
    if (pipe(fds) != 0) return 1;
    wl_data_offer_receive(current_offer, mime, fds[1]);
    wl_display_flush(display);
    close(fds[1]);

    char buffer[4096];
    ssize_t n = read(fds[0], buffer, sizeof(buffer) - 1);
    close(fds[0]);
    if (n < 0) n = 0;
    buffer[n] = 0;
    printf("%s", buffer);
    fflush(stdout);
    return 0;
}
