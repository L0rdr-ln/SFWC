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
```

Design notes:

- **Rendering** uses the wlroots scene graph (`wlr_scene`). Window = scene
  tree containing surface, decoration rects and shadow.
- **Stacking**: a linked list of views from bottom to top; focusing raises.
- **Animations** are small tweens (start, end, duration, easing) updated every
  frame callback; they animate scene node position/opacity/scale.
- **Config/themes** are parsed into plain structs; reload swaps the structs and
  re-applies them to existing views.
