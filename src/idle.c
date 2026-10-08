/* Idle inhibit. */
#include "server.h"

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
    struct idle_inhibitor *inh = wl_container_of(listener, inh, destroy);
    struct server *server = inh->server;
    wl_list_remove(&inh->destroy.link);
    free(inh);
    server->idle_inhibitors--;
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, server->idle_inhibitors > 0);
}

void server_new_idle_inhibitor(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inh = data;
    struct idle_inhibitor *inh = calloc(1, sizeof *inh);
    if (!inh) {
        return;
    }
    inh->server = server;
    inh->destroy.notify = idle_inhibitor_destroy;
    wl_signal_add(&wlr_inh->events.destroy, &inh->destroy);
    server->idle_inhibitors++;
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
}
