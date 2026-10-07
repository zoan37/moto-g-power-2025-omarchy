// Forward the Ilitek's first contact as a Wayland absolute click pointer.
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "virtual-pointer-client.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name,
             &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener listener = {global, removed};
static uint32_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

int main(void) {
    int input = -1;
    char path[64], name[128];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, "ILITEK_TDDI")) {
            input = fd; break;
        }
        close(fd);
    }
    if (input < 0) { perror("Ilitek event device"); return 1; }
    struct input_absinfo ax, ay;
    if (ioctl(input, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
        ioctl(input, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0 ||
        ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) {
        perror("touch axis extent"); return 1;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { perror("Hyprland Wayland connection"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager) {
        fputs("Virtual pointer protocol unavailable\n", stderr); return 1;
    }
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    if (ioctl(input, EVIOCGRAB, 1) < 0) { perror("touch grab"); return 1; }
    fprintf(stderr, "Ilitek Wayland pointer ready: %s, extent %dx%d\n", path,
            ax.maximum - ax.minimum, ay.maximum - ay.minimum);
    int slot = 0, x = ax.minimum, y = ay.minimum;
    bool touching = false, sent = false;
    struct pollfd fds[] = {{input, POLLIN, 0}, {wl_display_get_fd(display), POLLIN, 0}};
    for (;;) {
        if (wl_display_dispatch_pending(display) < 0 || wl_display_flush(display) < 0)
            break;
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & (POLLHUP | POLLERR)) break;
        if (fds[1].revents & POLLIN && wl_display_dispatch(display) < 0) break;
        if (!(fds[0].revents & POLLIN)) continue;
        struct input_event events[64];
        ssize_t count = read(input, events, sizeof(events));
        if (count < 0) { if (errno == EAGAIN) continue; break; }
        for (size_t i = 0; i < (size_t)count / sizeof(events[0]); i++) {
            struct input_event *event = &events[i];
            if (event->type == EV_ABS) {
                if (event->code == ABS_MT_SLOT) slot = event->value;
                else if (slot == 0) {
                    if (event->code == ABS_MT_TRACKING_ID) touching = event->value >= 0;
                    if (event->code == ABS_MT_POSITION_X) x = event->value;
                    if (event->code == ABS_MT_POSITION_Y) y = event->value;
                }
            } else if (event->type == EV_SYN && event->code == SYN_REPORT) {
                uint32_t now = milliseconds();
                if (touching) zwlr_virtual_pointer_v1_motion_absolute(pointer, now,
                    x - ax.minimum, y - ay.minimum, ax.maximum - ax.minimum, ay.maximum - ay.minimum);
                if (touching != sent) {
                    zwlr_virtual_pointer_v1_button(pointer, now, BTN_LEFT,
                        touching ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                    fprintf(stderr, "Touch contact %s\n", touching ? "down" : "up");
                    sent = touching;
                }
                zwlr_virtual_pointer_v1_frame(pointer);
                if (wl_display_flush(display) < 0) goto finish;
            }
        }
    }
finish:
    ioctl(input, EVIOCGRAB, 0);
    close(input);
    wl_display_disconnect(display);
    return 0;
}
