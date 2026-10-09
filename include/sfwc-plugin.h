/*
 * SFWC plugin API, version 2.
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
 *  - everything runs on the compositor's event loop thread; callbacks must be quick (frame runs
 *    once per output frame, 60 times a second or more, toplevel_commit on every client commit);
 *  - a window (struct sfwc_toplevel) is only valid until its toplevel_unmap callback has
 *    returned; drop every reference to it there;
 *  - when fini() returns the plugin must have removed everything it added to the scene graph and
 *    to the event loop: the shared object is unloaded right after.
 *
 * Compatibility: the host table and struct sfwc_toplevel_info only grow (fields are appended;
 * struct_size / size tell how much exists). struct sfwc_plugin carries its own struct_size, so a
 * plugin may lack the newest callbacks. The API version changes when something is removed or
 * changes meaning.
 */
#ifndef SFWC_PLUGIN_H
#define SFWC_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/version.h>

#define SFWC_PLUGIN_API_VERSION 2

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
    bool visible;    /* mapped, not minimized, on the visible workspace and its tree is enabled */
    bool maximized;
    bool fullscreen;
    bool moving;     /* the user drags it right now */
    bool busy;       /* a plugin (see toplevel_busy) is working on it: stay out of its way */
    /* appended in API 2 */
    bool minimized;
    bool shown;      /* should be visible (current workspace, not minimized), whatever its tree says
                      * right now: an animation may keep a tree shown a little longer */
    int workspace;   /* 0-based */
};

struct sfwc_host {
    uint32_t api_version;  /* SFWC_PLUGIN_API_VERSION of the compositor */
    uint32_t struct_size;  /* sizeof(struct sfwc_host) of the compositor */
    void *priv;            /* the compositor's, do not touch */

    /* The tree that holds all windows (beside it, effects can add their own trees). */
    struct wlr_scene_tree *windows_tree;
    /* Where the pointer is, in layout coordinates. */
    void (*cursor_position)(struct sfwc_host *host, double *x, double *y);

    /* The value of `key` in this plugin's [plugin:NAME] section (the last one if it is given more
     * than once), or NULL. The string stays valid until the config is reloaded (reconfigure
     * callback) or the plugin is unloaded. */
    const char *(*config_get)(struct sfwc_host *host, const char *key);

    /* Ask for another frame on every output, even if no client draws (animations). */
    void (*request_frame)(struct sfwc_host *host);

    /* Windows, top of the stack first. Pass NULL to get the first one; returns NULL at the end.
     * Only mapped windows are listed. */
    struct sfwc_toplevel *(*toplevel_next)(struct sfwc_host *host, struct sfwc_toplevel *prev);
    /* Fills `info`. Returns false for a window that is gone. */
    bool (*toplevel_info)(struct sfwc_host *host, const struct sfwc_toplevel *t, struct sfwc_toplevel_info *info);

    /* wlr_log() with the plugin's name in front. level: WLR_ERROR 1, WLR_INFO 2, WLR_DEBUG 3. */
    void (*log)(struct sfwc_host *host, int level, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

    /* appended in API 2 */

    /* All lines of this plugin's section, in file order, repeated keys included (animation rules
     * and curves are lists). `line` is the line number in the config file. */
    size_t (*config_count)(struct sfwc_host *host);
    bool (*config_entry)(struct sfwc_host *host, size_t index, const char **key, const char **value, int *line);
    /* The output (monitor) that contains the point, or the nearest one; {0} if there is none. */
    void (*output_box_at)(struct sfwc_host *host, double x, double y, struct wlr_box *out);
    /* The visible workspace, 0-based. */
    int (*current_workspace)(struct sfwc_host *host);
    /* The compositor's monotonic clock in ms, the same that `frame` gets. */
    uint32_t (*now_ms)(struct sfwc_host *host);
    /* Mark a window as animated by you (+1) or not any more (-1): other plugins see info.busy and
     * keep out of the way. Balance the calls; unmap clears the count. */
    void (*toplevel_busy)(struct sfwc_host *host, struct sfwc_toplevel *t, int delta);
    /* The look of the frame colors: 0 = unfocused, 1 = focused, in between = a blend. Use it to
     * animate the change (see toplevel_focus). */
    void (*toplevel_set_focus_mix)(struct sfwc_host *host, struct sfwc_toplevel *t, double mix);
};

/* What a plugin exports. Every callback is optional except `init`. */
struct sfwc_plugin {
    uint32_t api_version;       /* SFWC_PLUGIN_API_VERSION this plugin was written for */
    uint32_t struct_size;       /* sizeof(struct sfwc_plugin): set it to that, callbacks beyond are ignored */
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
    /* The window is about to be hidden (unmapped) or destroyed. Called before the window leaves
     * the list, while its scene tree and buffers are still there. */
    void (*toplevel_unmap)(struct sfwc_host *host, struct sfwc_toplevel *t);

    /* appended in API 2 */

    /* A window was mapped, placed and focused; its frame exists. */
    void (*toplevel_map)(struct sfwc_host *host, struct sfwc_toplevel *t);
    /* The client committed a new state of a mapped window (often: be quick). */
    void (*toplevel_commit)(struct sfwc_host *host, struct sfwc_toplevel *t);
    /* The compositor moves the window (maximize, restore, to the next output) from tree position
     * (from_x, from_y) to (to_x, to_y). Return true to animate it: the tree stays at `from` and you
     * move it, ending exactly at `to`. Return false (or have no callback) and it jumps. */
    bool (*toplevel_move)(struct sfwc_host *host, struct sfwc_toplevel *t, double from_x, double from_y,
                          double to_x, double to_y);
    /* Something takes over the window (the user starts to drag it, ...): finish what you animate
     * on it at once, leaving it in its normal state. */
    void (*toplevel_cancel)(struct sfwc_host *host, struct sfwc_toplevel *t);
    /* The frame colors should go from the look `from` to `to` (0 unfocused, 1 focused). Return
     * true to take over: call toplevel_set_focus_mix() every frame until you reach `to`. Return
     * false and the colors switch at once. */
    bool (*toplevel_focus)(struct sfwc_host *host, struct sfwc_toplevel *t, double from, double to);
    /* A workspace switch: `leaving` is called first (finish animations of the previous switch),
     * then the windows are shown and hidden for the new workspace, then `entered`. To let the old
     * workspace slide out, enable the trees of its windows again in `entered` and disable them
     * when you are done (info.shown says whether they should be visible). */
    void (*workspace_leaving)(struct sfwc_host *host, int old_ws, int new_ws);
    void (*workspace_entered)(struct sfwc_host *host, int old_ws, int new_ws);
    /* A layer-shell surface (panel, launcher, notification) appeared / is about to go away. `tree`
     * is its scene tree. */
    void (*layer_map)(struct sfwc_host *host, struct wlr_scene_tree *tree);
    void (*layer_unmap)(struct sfwc_host *host, struct wlr_scene_tree *tree);
};

/* The one symbol a plugin exports. */
#define SFWC_PLUGIN_ENTRY_SYMBOL "sfwc_plugin_entry"
typedef const struct sfwc_plugin *(*sfwc_plugin_entry_fn)(void);

#define SFWC_PLUGIN_EXPORT __attribute__((visibility("default")))

#endif
