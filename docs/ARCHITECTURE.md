# Architecture

SFWC is a single-process C program on top of wlroots 0.20.

The compositor is split by topic. All modules share `src/server.h` (the `struct server`,
`struct toplevel`, `struct output`, ... and the functions the modules call from each other);
everything else in a module is `static`.

```
src/main.c         startup: display, backend, renderer, protocols, socket, autostart, main loop
src/settings.c     config + theme loading, live reload (inotify), theme templates, spawn
src/window.c       xdg-shell windows: placement, focus, maximize/fullscreen/minimize, popups
src/workspace.c    workspaces: what is shown, switching, sending windows
src/decoration.c   server-side decorations: titlebar, borders, shadow as scene buffers
src/output.c       monitors: layout, work area, "next output" helpers, frame scheduling
src/layers.c       wlr-layer-shell: panels, wallpapers, launchers, exclusive zones
src/input.c        seat, keyboards (xkb, repeat), virtual input (tests), selection requests
src/actions.c      what a keybind does (dispatch_action)
src/cursor.c       pointer: hit testing, focus, drag to move/resize, buttons, snapping
src/protocols.c    small protocols: viewporter, fractional-scale, cursor-shape, xdg-activation,
                   pointer constraints + relative pointer
src/output_mgmt.c  wlr-output-management (kanshi, wlr-randr)
src/pointer_config.c  libinput settings from [input]
src/lock.c         ext-session-lock
src/foreign.c      wlr-foreign-toplevel-management (taskbars)
src/idle.c         idle inhibit

Parsers and helpers without a wlroots dependency (unit-tested in tests/):
src/inifile.c      line-numbered INI reader on top of the vendored inih
src/config.c       sfwc.conf: sections, keybinds, defaults
src/theme.c        *.theme files; the built-in default theme
src/deco.c         frame geometry, hit testing and cairo/pango drawing of the decorations
src/anim.c         easing and time based progress
src/template.c     @section.key@ rendering of theme values
src/sfwc-theme-apply.c   renders themes/templates/*.in -> $XDG_RUNTIME_DIR/sfwc/
                   (separate program; part of the compositor package, templates come
                   with the optional theme package)
```

Adding a protocol: create the global in `main()`, put its handlers in a module of its own (or
the one it belongs to) and give `server.h` the few functions other modules need.

Design notes:

- **Rendering** uses the wlroots scene graph (`wlr_scene`). Window = scene
  tree containing surface, decoration rects and shadow.
- **Stacking**: a linked list of views from bottom to top; focusing raises.
- **Animations** are a plugin (sfwc-plugins); the compositor only offers hooks for them.
- **Config/themes** are parsed into plain structs; reload swaps the structs and
  re-applies them to existing views.

## Protocols the compositor must implement

Companion tools only work if the matching protocols exist. Implemented: `xdg-shell`,
`xdg-decoration`, `xdg-output`, `wlr-layer-shell` (bar, wallpaper, launcher), `ext-session-lock`
(lock screen), `ext-idle-notify` and idle inhibit (idle daemon),
`wlr-foreign-toplevel-management` (taskbar), `wlr-screencopy` (screenshots), primary selection
and `wlr-data-control` (clipboard managers), `viewporter`, `fractional-scale-v1`, `cursor-shape-v1`,
`xdg-activation-v1`, `pointer-constraints` + `relative-pointer`, `wlr-output-management`. Not yet: a workspace protocol (`ext-workspace`,
which wlroots 0.20 now provides), `ext-image-copy-capture` (wlroots 0.19+), Xwayland.

## Libraries beyond wlroots

wayland-protocols, xkbcommon, libxcursor (cursor themes), cairo + pango (title
text in decorations), inih (config/theme parser, vendored). libinput, libdrm
and seatd come in through wlroots. Pin wlroots to 0.20; it changes API every
minor release. Debug builds: `meson setup build -Db_sanitize=address,undefined`.

Config reload uses inotify on the config **directory** (editors replace files
via rename), wired into the Wayland event loop with `wl_event_loop_add_fd`.

## Plugins

`src/plugin.c` is the host side of the plugin API in `include/sfwc-plugin.h` (see PLUGINS.md):
it loads `NAME.so` for every `load = NAME` in `[plugins]`, hands each plugin a `struct sfwc_host`
(a table of functions, no internal structs), and calls its `frame` callback from
`output_frame()`; the window hooks (`toplevel_map`, `toplevel_commit`, `toplevel_unmap`,
`toplevel_move`, `toplevel_cancel`, `toplevel_focus`) from `window.c`, `cursor.c` and `decoration.c`; the
workspace hooks from `workspace.c`; the layer hooks from `layers.c`; `reconfigure` from
`reload_config()`; `fini` at shutdown.
`plugins/` holds two test plugins (`hooktest`, `badplugin`); real plugins live in the
sfwc-plugins repository. wlroots is built or linked as a shared library so that plugins share
the compositor's copy.
