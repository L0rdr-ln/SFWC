/*
 * SFWC plugin API, version 1.
 *
 * A plugin is a shared object that sfwc loads at start (and on config reload) when the config
 * lists it:
 *
 *     [plugins]
 *     load = wobbly
 *     [plugin:wobbly]
 *     spring = 120
 *
 * It is searched as NAME.so in $SFWC_PLUGIN_PATH (colon separated), ~/.local/share/sfwc/plugins
 * and the install directory (<libdir>/sfwc/plugins), in that order.
 *
 * A plugin runs inside the compositor with the compositor's rights: only load code you trust.
 *
 * The plugin talks to the compositor in two ways: it exports one function, sfwc_plugin_entry(),
 * that returns a table of callbacks (struct sfwc_plugin), and it receives a struct sfwc_host with
 * the functions it may call. Neither side uses the compositor's internal structs. The wlroots
 * scene graph types are part of the interface (effects draw into it), so a plugin must be built
 * against the same wlroots version as sfwc; sfwc refuses plugins that say otherwise.
 *
 * Rules:
 *  - everything runs on the compositor's event loop thread; callbacks must be quick (they run
 *    once per output frame, 60 times a second or more);
 *  - a window (struct sfwc_toplevel) is only valid until its toplevel_unmap callback has
 *    returned; drop every reference to it there;
 *  - when fini() returns the plugin must have removed everything it added to the scene graph and
 *    to the event loop: the shared object is unloaded right after.
 *
 * New fields are only ever appended to the structs; sfwc_host.struct_size tells a plugin how much
 * of the host table exists. The API version changes when something is removed or changes meaning.
 */
#ifndef SFWC_PLUGIN_H
#define SFWC_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/version.h>

#define SFWC_PLUGIN_API_VERSION 1

struct sfwc_host;

/* A window. Opaque. */
struct sfwc_toplevel;

/* What a plugin may know about a window; filled by sfwc_host.toplevel_info(). */
struct sfwc_toplevel_info {
    uint32_t size;                /* sizeof(struct sfwc_toplevel_info) of the host, on return */
    struct wlr_box outer;         /* the window with its frame (title bar, borders), layout coordinates */
    struct wlr_scene_tree *tree;  /* holds the window's surfaces and frame; its position is the window's */
    struct wlr_scene_buffer *shadow; /* the drop shadow, or NULL; it should not be deformed */
    struct wlr_scene_buffer *frame;  /* the frame's picture, or NULL; transparent inside `inner` */
    struct wlr_box inner;         /* the part of `outer` that belongs to the client, layout coordinates */
    bool mapped;
    bool visible;    /* mapped, not minimized and on the visible workspace */
    bool maximized;
    bool fullscreen;
    bool moving;     /* the user drags it right now */
    bool busy;       /* the compositor's own animation is working on it: stay out of its way */
};

struct sfwc_host {
    uint32_t api_version;  /* SFWC_PLUGIN_API_VERSION of the compositor */
    uint32_t struct_size;  /* sizeof(struct sfwc_host) of the compositor */
    void *priv;            /* the compositor's, do not touch */

    /* The tree that holds all windows (beside it, effects can add their own trees). */
    struct wlr_scene_tree *windows_tree;
    /* Where the pointer is, in layout coordinates. */
    void (*cursor_position)(struct sfwc_host *host, double *x, double *y);

    /* The value of `key` in this plugin's [plugin:NAME] section, or NULL. The string stays valid
     * until the config is reloaded (reconfigure callback) or the plugin is unloaded. */
    const char *(*config_get)(struct sfwc_host *host, const char *key);

    /* Ask for another frame on every output, even if no client draws (animations). */
    void (*request_frame)(struct sfwc_host *host);

    /* Windows, top of the stack first. Pass NULL to get the first one; returns NULL at the end. */
    struct sfwc_toplevel *(*toplevel_next)(struct sfwc_host *host, struct sfwc_toplevel *prev);
    /* Fills `info`. Returns false for a window that is gone. */
    bool (*toplevel_info)(struct sfwc_host *host, const struct sfwc_toplevel *t, struct sfwc_toplevel_info *info);

    /* wlr_log() with the plugin's name in front. level: WLR_ERROR 1, WLR_INFO 2, WLR_DEBUG 3. */
    void (*log)(struct sfwc_host *host, int level, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
};

/* What a plugin exports. Every callback is optional except `init`. */
struct sfwc_plugin {
    uint32_t api_version;       /* SFWC_PLUGIN_API_VERSION this plugin was written for */
    const char *name;           /* the NAME it is loaded as */
    const char *wlroots_version; /* WLR_VERSION_STR it was built against */

    /* Return false to say "I cannot work": the plugin is unloaded and a message is logged. */
    bool (*init)(struct sfwc_host *host);
    /* Remove everything you added, free everything. */
    void (*fini)(struct sfwc_host *host);
    /* The config was reloaded: read your [plugin:NAME] settings again. */
    void (*reconfigure)(struct sfwc_host *host);
    /* Once per output frame, before the frame is committed. `now_ms` is a monotonic clock. */
    void (*frame)(struct sfwc_host *host, uint32_t now_ms);
    /* The window is about to be hidden (unmapped) or destroyed. */
    void (*toplevel_unmap)(struct sfwc_host *host, struct sfwc_toplevel *t);
};

/* The one symbol a plugin exports. */
#define SFWC_PLUGIN_ENTRY_SYMBOL "sfwc_plugin_entry"
typedef const struct sfwc_plugin *(*sfwc_plugin_entry_fn)(void);

#define SFWC_PLUGIN_EXPORT __attribute__((visibility("default")))

#endif
