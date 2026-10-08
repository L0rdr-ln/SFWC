/* Startup, option parsing and the main loop. */
#include "server.h"

/* ----------------------------------------------------------------- main */

/* Testing aid: SFWC_TEST_OUTPUTS=1024x600,800x600 adds headless outputs. */
static void add_test_outputs(struct wlr_backend *backend, void *data)
{
    if (!wlr_backend_is_headless(backend)) {
        return;
    }
    char *spec = strdup(data), *save = NULL;
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned w, h;
        if (sscanf(tok, "%ux%u", &w, &h) == 2) {
            wlr_headless_add_output(backend, w, h);
        }
    }
    free(spec);
}

static int handle_signal(int signo, void *data)
{
    wlr_log(WLR_INFO, "signal %d, shutting down", signo);
    wl_display_terminate(data);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-s startup-command] [-h]\n", argv0);
}

int main(int argc, char *argv[])
{
    char *startup_cmd = NULL;
    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        default:
            usage(argv[0]);
            return c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* SFWC_LOG_LEVEL=debug|info|error|silent (default info) */
    enum wlr_log_importance log_level = WLR_INFO;
    const char *lvl = getenv("SFWC_LOG_LEVEL");
    if (lvl && !strcmp(lvl, "debug")) {
        log_level = WLR_DEBUG;
    } else if (lvl && !strcmp(lvl, "error")) {
        log_level = WLR_ERROR;
    } else if (lvl && !strcmp(lvl, "silent")) {
        log_level = WLR_SILENT;
    }
    wlr_log_init(log_level, NULL);

    struct server server = {0};
    server.display = wl_display_create();
    if (!server.display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return EXIT_FAILURE;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    wl_event_loop_add_signal(loop, SIGINT, handle_signal, server.display);
    wl_event_loop_add_signal(loop, SIGTERM, handle_signal, server.display);
    init_config(&server, loop);

    server.backend = wlr_backend_autocreate(loop, &server.session);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return EXIT_FAILURE;
    }

    const char *test_outputs = getenv("SFWC_TEST_OUTPUTS");
    if (test_outputs && *test_outputs) {
        if (wlr_backend_is_multi(server.backend)) {
            wlr_multi_for_each_backend(server.backend, add_test_outputs, (void *)test_outputs);
        } else {
            add_test_outputs(server.backend, (void *)test_outputs);
        }
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return EXIT_FAILURE;
    }
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return EXIT_FAILURE;
    }

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
    wlr_screencopy_manager_v1_create(server.display);

    server.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.display);
    server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
    wl_signal_add(&server.xdg_decoration_mgr->events.new_toplevel_decoration,
                  &server.new_toplevel_decoration);
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    /* z-order, bottom to top: background, bottom, windows, top, overlay */
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&server.scene->tree);
    server.windows_tree = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&server.scene->tree);
    server.lock_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);
    server.lock_bg = wlr_scene_rect_create(server.lock_tree, 0, 0, (float[4]){0, 0, 0, 1});
    wl_list_init(&server.layer_surfaces);
    server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    wl_list_init(&server.toplevels);
    wl_list_init(&server.animations);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    /* Cursor theme/size follow XCURSOR_THEME / XCURSOR_SIZE like other desktops. */
    unsigned cursor_size = 24;
    const char *size_env = getenv("XCURSOR_SIZE");
    if (size_env && atoi(size_env) > 0) {
        cursor_size = (unsigned)atoi(size_env);
    } else {
        setenv("XCURSOR_SIZE", "24", 0);
    }
    server.cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cursor_size);
    server.cursor_mode = CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.display, "seat0");
    server.keyboard_focus_change.notify = handle_keyboard_focus_change;
    wl_signal_add(&server.seat->keyboard_state.events.focus_change, &server.keyboard_focus_change);
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    /* middle-click paste, clipboard managers, idle daemons, taskbars */
    wlr_primary_selection_v1_device_manager_create(server.display);
    server.request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection,
                  &server.request_set_primary_selection);
    wlr_data_control_manager_v1_create(server.display);
    server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.display);
    server.new_idle_inhibitor.notify = server_new_idle_inhibitor;
    wl_signal_add(&server.idle_inhibit_mgr->events.new_inhibitor, &server.new_idle_inhibitor);
    server.foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server.display);
    server.lock_mgr = wlr_session_lock_manager_v1_create(server.display);
    server.new_lock.notify = server_new_lock;
    wl_signal_add(&server.lock_mgr->events.new_lock, &server.new_lock);

    const char *vinput = getenv("SFWC_ENABLE_VIRTUAL_INPUT");
    if (vinput && strcmp(vinput, "1") == 0) {
        wlr_log(WLR_INFO, "virtual input protocols enabled (testing)");
        server.virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(server.display);
        server.new_virtual_pointer.notify = server_new_virtual_pointer;
        wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer,
                      &server.new_virtual_pointer);
        server.virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(server.display);
        server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
        wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
                      &server.new_virtual_keyboard);
    }

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return EXIT_FAILURE;
    }
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return EXIT_FAILURE;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    /* what portals and toolkits look at; keep values the user or the login manager set */
    setenv("XDG_CURRENT_DESKTOP", "SFWC", 0);
    setenv("XDG_SESSION_TYPE", "wayland", 0);
    if (!getenv("SFWC_NO_DBUS_ENV")) { /* tell D-Bus activated programs (portals) where we are */
        spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
              "XDG_SESSION_TYPE >/dev/null 2>&1");
    }
    for (size_t i = 0; i < server.config.n_autostart; i++) {
        char *cmd = expand_command(&server, server.config.autostart[i]);
        wlr_log(WLR_INFO, "autostart: %s", cmd);
        spawn(cmd);
        free(cmd);
    }
    if (startup_cmd) {
        spawn(startup_cmd);
    }
    wlr_log(WLR_INFO, "sfwc running on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.new_idle_inhibitor.link);
    wl_list_remove(&server.new_lock.link);
    if (server.cur_lock) {
        wl_list_remove(&server.lock_new_surface.link);
        wl_list_remove(&server.lock_unlock.link);
        wl_list_remove(&server.lock_destroy.link);
    }
    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_toplevel_decoration.link);
    wl_list_remove(&server.new_layer_surface.link);
    wl_list_remove(&server.keyboard_focus_change.link);
    if (server.virtual_pointer_mgr) {
        wl_list_remove(&server.new_virtual_pointer.link);
        wl_list_remove(&server.new_virtual_keyboard.link);
    }

    struct animation *anim, *anim_tmp;
    wl_list_for_each_safe(anim, anim_tmp, &server.animations, link) {
        wl_list_remove(&anim->link);
        free(anim);
    }
    if (server.inotify_source) {
        wl_event_source_remove(server.inotify_source);
    }
    if (server.inotify_fd >= 0) {
        close(server.inotify_fd);
    }
    free(server.config_path);
    free(server.config_name);
    config_finish(&server.config);
    theme_finish(&server.theme);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return EXIT_SUCCESS;
}
