/*
 * SFWC - Simple Floating Wayland Compositor
 *
 * Skeleton: creates a display, backend and renderer, then runs the event
 * loop. Window management, input, config, themes and animations are tracked
 * in docs/ROADMAP.md.
 */
#include <stdio.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>

int main(void)
{
    wlr_log_init(WLR_INFO, NULL);

    struct wl_display *display = wl_display_create();
    if (!display) {
        return EXIT_FAILURE;
    }

    struct wlr_backend *backend =
        wlr_backend_autocreate(wl_display_get_event_loop(display), NULL);
    if (!backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return EXIT_FAILURE;
    }

    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    if (!renderer || !wlr_renderer_init_wl_display(renderer, display)) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return EXIT_FAILURE;
    }

    struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return EXIT_FAILURE;
    }

    const char *socket = wl_display_add_socket_auto(display);
    if (!socket || !wlr_backend_start(backend)) {
        wlr_log(WLR_ERROR, "failed to start backend or open socket");
        wlr_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "sfwc running on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(display);

    wl_display_destroy_clients(display);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return EXIT_SUCCESS;
}
