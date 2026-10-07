// Exercise the same Wayland pointer protocol used by the touch bridge.
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>
#include "virtual-pointer-client.h"
#include <string.h>
static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener listener = {global, removed};
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned x = strtoul(argv[1], NULL, 10), y = strtoul(argv[2], NULL, 10);
    if (x > 10000 || y > 10000) return 2;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager) return 1;
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 1, x, y, 10000, 10000);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    zwlr_virtual_pointer_v1_button(pointer, 2, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(70000);
    zwlr_virtual_pointer_v1_button(pointer, 72, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    puts("Wayland click sent");
    return 0;
}
