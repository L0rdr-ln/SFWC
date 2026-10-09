# Plugins

The compositor core stays small. Anything that is "nice to have" (effects, extra behavior) can
live in a plugin: a shared object that sfwc loads when the config asks for it. A system without
plugins pays for the loader (about 250 lines) and nothing else.

Shipped plugins:

| plugin | what it does |
|---|---|
| `wobbly` | windows wobble like jelly when they move ([settings](#wobbly)) |

## Using plugins

```ini
[plugins]
load = wobbly            # one line per plugin

[plugin:wobbly]          # settings; every plugin documents its own keys
spring = 120
friction = 9
```

A plugin named `NAME` is the file `NAME.so`, searched in

1. every directory of `$SFWC_PLUGIN_PATH` (colon separated),
2. `$XDG_DATA_HOME/sfwc/plugins` (default `~/.local/share/sfwc/plugins`),
3. the install directory (`<prefix>/<libdir>/sfwc/plugins`, e.g. `/usr/local/lib/sfwc/plugins`).

The config is reloaded live: a plugin that is added to the list is loaded, one that is removed is
unloaded, the others get a `reconfigure` callback and read their settings again. A plugin that is
missing, refuses to start (`init` returns false), was written for another API version or was
built against another wlroots version is logged and skipped; the compositor carries on.

Names are 1 to 32 characters of `a-z 0-9 _ -`; paths are not accepted. A file that everybody can
write to is not loaded.

**Trust.** A plugin runs inside the compositor with your rights and can do anything the
compositor can: read your windows, log keystrokes it is given, run programs. Only install plugins
you would also run as programs. (The config file is no safer: `spawn:` and `[autostart]` already
run commands.)

## wobbly

Windows are drawn on a grid of springs. When a window moves (dragging, maximize, snapping, "next
output") the grid lags behind and swings back. While a window is dragged, the part near the
pointer follows closely and the far side trails.

| key | default | |
|---|---|---|
| `grid` | 6 | nodes per side of the mesh, 3 to 12 |
| `spring` | 120 | how hard a node is pulled back to its place, 1 to 1000 |
| `friction` | 9 | damping, 0.5 to 100. About `2 * sqrt(spring)` is critical (no overshoot); less swings longer, more is sluggish |

How it works and its limits: wlroots' scene graph cannot warp a picture, so while a window swings
it is cut into tiles of about 40 px that are moved and stretched along the mesh; the window's own
pictures stay in place, invisible, so input and frame callbacks keep working. The edges of the
tiles are straight, so large stretches can show faint seams, and big windows cost some CPU while
they swing. A window that is being resized does not wobble. The sfwc animations and wobbling do
not mix: a window that is fading in is left alone.

## Writing a plugin

A plugin includes `sfwc-plugin.h` (installed to `<prefix>/include/sfwc/`; for plugins outside this
repository `pkg-config --cflags sfwc-plugin` finds it, and `pkg-config --variable=plugindir
sfwc-plugin` says where to install). It exports one function and gets a table of host functions.

```c
#include <sfwc-plugin.h>

static bool on_init(struct sfwc_host *host)
{
    host->log(host, 2, "hello, greeting=%s", host->config_get(host, "greeting"));
    return true;
}

static void on_frame(struct sfwc_host *host, uint32_t now_ms)
{
    for (struct sfwc_toplevel *t = host->toplevel_next(host, NULL); t; t = host->toplevel_next(host, t)) {
        struct sfwc_toplevel_info info;
        if (host->toplevel_info(host, t, &info) && info.visible) {
            /* info.outer is the window with its frame, info.tree its scene tree */
        }
    }
}

static const struct sfwc_plugin plugin = {
    .api_version = SFWC_PLUGIN_API_VERSION,
    .name = "hello",
    .wlroots_version = WLR_VERSION_STR,
    .init = on_init,
    .frame = on_frame,
};

SFWC_PLUGIN_EXPORT const struct sfwc_plugin *sfwc_plugin_entry(void) { return &plugin; }
```

Build it as a shared module (`-shared -fPIC`, or meson's `shared_module(..., name_prefix: '')`)
against **the same wlroots version as the compositor**; the loader compares `WLR_VERSION_STR` and
refuses anything else. Link wlroots dynamically so that the plugin and the compositor use the same
copy. `plugins/hooktest/hooktest.c` is a complete small plugin; `plugins/wobbly/` is a real one.

### Callbacks

| callback | when |
|---|---|
| `init(host)` | after loading; return false to refuse |
| `fini(host)` | before unloading (config change or shutdown): remove everything you added to the scene graph and the event loop |
| `reconfigure(host)` | the config was reloaded: read your settings again |
| `frame(host, now_ms)` | before every output frame is committed |
| `toplevel_unmap(host, t)` | a window goes away (or is hidden by being closed): drop your references; restore anything you changed on its buffers, because the close animation takes its picture right after |

### Host functions

`windows_tree`, `cursor_position`, `config_get`, `request_frame`, `toplevel_next`,
`toplevel_info` and `log`; see the header for what each does. Windows are opaque handles; the
information is copied into a `struct sfwc_toplevel_info` (outer box, scene tree, shadow and frame
buffers, the client's inner box, visible/maximized/fullscreen/moving/busy).

### Rules

- Everything runs on the compositor's single thread; `frame` runs 60 or more times a second per
  output, so keep it cheap.
- A plugin must not keep pointers into the host's data past the call that returned them, except
  windows until their `toplevel_unmap`.
- The API only grows: new fields are appended and `host->struct_size` tells how much exists.
  Anything removed or changed in meaning bumps `SFWC_PLUGIN_API_VERSION`.
- The wlroots scene graph types are part of the interface. wlroots has no stable ABI, so a
  compositor update to a new wlroots needs plugins rebuilt.
