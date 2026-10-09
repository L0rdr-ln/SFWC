# Plugins

The compositor core stays small. Anything that is "nice to have" (effects, extra behavior) can
live in a plugin: a shared object that sfwc loads when the config asks for it. A system without
plugins pays for the loader (about 250 lines) and nothing else.

The plugins themselves live in their own repository,
[sfwc-plugins](https://github.com/L0rdr-ln/sfwc-plugins), so that a minimal install carries none of
them:

| plugin | what it does |
|---|---|
| `wobbly` | windows wobble like jelly when they move |
| `animations` | open/close/move animations, workspace slides, border fade, fire, squeeze, zoom (Hyprland style rules in `[animations]`) |

Build and install them with `meson setup build && meson install -C build` there; each plugin's
settings are described in that repository's README.

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
copy. `plugins/hooktest/hooktest.c` is a complete small plugin; `wobbly` in sfwc-plugins is a real one.

### Callbacks

Everything is optional except `init`. Set `struct_size` to `sizeof(struct sfwc_plugin)`: callbacks
beyond the size the compositor knows are ignored, and a table that ends early just lacks the later
callbacks.

| callback | when |
|---|---|
| `init(host)` | after loading; return false to refuse |
| `fini(host)` | before unloading (config change or shutdown): remove everything you added to the scene graph and the event loop |
| `reconfigure(host)` | the config was reloaded: read your settings again |
| `frame(host, now_ms)` | before every output frame is committed |
| `toplevel_map(host, t)` | a window was mapped, placed and focused; its frame exists |
| `toplevel_commit(host, t)` | the client committed new content of a mapped window (often: be quick) |
| `toplevel_unmap(host, t)` | a window goes away: drop your references. It runs while the window's pictures still exist (close animations copy them here); restore anything you changed on its buffers |
| `toplevel_move(host, t, from, to)` | the compositor moves the window (maximize, restore, next output). Return true to animate it yourself: the tree stays at `from`, you end exactly at `to` |
| `toplevel_cancel(host, t)` | something takes the window over (a drag starts, another move): finish what you animate on it at once |
| `toplevel_focus(host, t, from, to)` | the frame colors should go from the look `from` to `to` (0 unfocused, 1 focused). Return true and call `toplevel_set_focus_mix()` each frame until you reach `to` |
| `workspace_leaving(host, old, new)` / `workspace_entered(host, old, new)` | around a workspace switch: first finish the previous switch's animation; after the windows were shown and hidden for the new workspace, enable the old windows' trees again to let them slide out and disable them when done |
| `layer_map(host, tree)` / `layer_unmap(host, tree)` | a layer-shell surface (panel, launcher, notification) appeared / goes away |

### Host functions

`windows_tree`, `cursor_position`, `config_get` (last value of a key), `config_count` /
`config_entry` (all lines of the section in order, with line numbers: for lists such as animation
rules), `request_frame`, `toplevel_next`, `toplevel_info`, `output_box_at`, `current_workspace`,
`now_ms`, `toplevel_busy` (+1/-1 while you animate a window: other plugins see `info.busy` and
keep out of the way), `toplevel_set_focus_mix` and `log`; see the header for what each does.
Windows are opaque handles; the information is copied into a `struct sfwc_toplevel_info` (outer
box, scene tree, shadow and frame buffers, the client's inner box, visible/shown/minimized/
maximized/fullscreen/moving/busy, workspace).

### Rules

- Everything runs on the compositor's single thread; `frame` runs 60 or more times a second per
  output, so keep it cheap.
- A plugin must not keep pointers into the host's data past the call that returned them, except
  windows until their `toplevel_unmap`.
- The API only grows: new fields and callbacks are appended and `host->struct_size` /
  `info.size` tell how much exists. Anything removed or changed in meaning bumps
  `SFWC_PLUGIN_API_VERSION` (now 2; version 1 plugins are refused and must be rebuilt).
- The wlroots scene graph types are part of the interface. wlroots has no stable ABI, so a
  compositor update to a new wlroots needs plugins rebuilt.
