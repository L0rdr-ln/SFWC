# Architecture

SFWC is a single-process C program on top of wlroots 0.18.

```
main.c        startup: display, backend, renderer, allocator, socket
(planned)
server.c      global state, event loop wiring
output.c      monitors: layout, frame scheduling
view.c        xdg-toplevel windows: stacking order, move/resize, focus
decor.c       server-side decorations drawn from the active theme
input.c       seat, keyboard, pointer, keybinds
config.c      parser for sfwc.conf, reload handling
theme.c       parser for *.theme files; built-in fallback theme, files come from sfwc-themes
anim.c        animation engine (tweens driven by the frame clock)
spawn.c       fork/exec with setsid, SIGCHLD reaping, autostart
(helper)
sfwc-theme-apply   renders themes/templates/*.in -> $XDG_RUNTIME_DIR/sfwc/
                   (separate program, part of the optional theme package)
```

Design notes:

- **Rendering** uses the wlroots scene graph (`wlr_scene`). Window = scene
  tree containing surface, decoration rects and shadow.
- **Stacking**: a linked list of views from bottom to top; focusing raises.
- **Animations** are small tweens (start, end, duration, easing) updated every
  frame callback; they animate scene node position/opacity/scale.
- **Config/themes** are parsed into plain structs; reload swaps the structs and
  re-applies them to existing views.

## Protocols the compositor must implement

Companion tools only work if the matching protocols exist:
`xdg-shell`, `xdg-decoration`, `wlr-layer-shell` (bar, wallpaper, launcher),
`ext-session-lock` (lock screen), `ext-idle-notify` (idle daemon),
`wlr-foreign-toplevel-management` (taskbar), a workspace protocol
(`ext-workspace`), plus `wlr-screencopy`/`ext-image-copy-capture` for screenshots.

## Libraries beyond wlroots

wayland-protocols, xkbcommon, libxcursor (cursor themes), cairo + pango (title
text in decorations), inih (config/theme parser, vendored). libinput, libdrm
and seatd come in through wlroots. Pin wlroots to 0.18; it changes API every
minor release. Debug builds: `meson setup build -Db_sanitize=address,undefined`.

Config reload uses inotify on the config **directory** (editors replace files
via rename), wired into the Wayland event loop with `wl_event_loop_add_fd`.
