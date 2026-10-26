// A virtual pointer for a headless wlroots compositor (#2724): sway and labwc started with
// WLR_BACKENDS=headless have no input device, so their seat offers clients no pointer at all, and neither
// `swaymsg seat ... cursor` nor `wlrctl pointer` (whose device lives only for one command) can reach a
// client. This holds one zwlr_virtual_pointer_v1 for as long as it runs and takes commands on stdin, one
// per line, answering "ok" after each so a script can step in time with it:
//   abs X Y W H    move to (X, Y) of a W x H layout (the output's size, for one output)
//   move DX DY     move by (DX, DY)
//   press | release   the left button
//   quit
// Built by tools/wayland-matrix.sh with wayland-scanner and wayland-client; never part of TaskSmack.

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#define BTN_LEFT 0x110

static struct wl_seat* g_Seat;
static struct zwlr_virtual_pointer_manager_v1* g_Manager;

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void) data;
    (void) version;
    if (strcmp(interface, wl_seat_interface.name) == 0 && g_Seat == NULL)
    {
        g_Seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    }
    else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
    {
        g_Manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}

static void onGlobalRemove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void) data;
    (void) registry;
    (void) name;
}

static const struct wl_registry_listener REGISTRY = {.global = onGlobal, .global_remove = onGlobalRemove};

static uint32_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(void)
{
    struct wl_display* display = wl_display_connect(NULL);
    if (display == NULL)
    {
        fputs("virtual-pointer: no Wayland display (WAYLAND_DISPLAY)\n", stderr);
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &REGISTRY, NULL);
    wl_display_roundtrip(display);
    if (g_Seat == NULL || g_Manager == NULL)
    {
        fputs("virtual-pointer: the compositor has no zwlr_virtual_pointer_manager_v1 or no seat\n", stderr);
        return 1;
    }
    struct zwlr_virtual_pointer_v1* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_Manager, g_Seat);
    wl_display_roundtrip(display);
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("ready");

    char line[256];
    while (fgets(line, sizeof line, stdin) != NULL)
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (sscanf(line, "abs %d %d %d %d", &x, &y, &w, &h) == 4 && w > 0 && h > 0)
        {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, nowMs(), (uint32_t) x, (uint32_t) y, (uint32_t) w, (uint32_t) h);
        }
        else if (sscanf(line, "move %d %d", &x, &y) == 2)
        {
            zwlr_virtual_pointer_v1_motion(pointer, nowMs(), wl_fixed_from_int(x), wl_fixed_from_int(y));
        }
        else if (strncmp(line, "press", 5) == 0 || strncmp(line, "release", 7) == 0)
        {
            const uint32_t state = line[0] == 'p' ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
            zwlr_virtual_pointer_v1_button(pointer, nowMs(), BTN_LEFT, state);
        }
        else if (strncmp(line, "quit", 4) == 0)
        {
            break;
        }
        else
        {
            fprintf(stderr, "virtual-pointer: unknown command: %s", line);
            puts("error");
            continue;
        }
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_roundtrip(display);
        puts("ok");
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
