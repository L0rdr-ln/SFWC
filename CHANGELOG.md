# Changelog

Versions follow the roadmap milestones while the project is pre-1.0:
`0.<milestone>.0` (v0.2.0 = M2). Dates are UTC.

## Unreleased

### Added
- Desktop protocols programs expect: `viewporter` and `fractional-scale` (sharp HiDPI: clients get
  the scale of the output they are on, 1.5 and so on), `cursor-shape` (named cursors from the
  compositor's theme), `xdg-activation` (a program can ask for a window to be focused, with a
  token; a window on another workspace is not pulled over), `pointer-constraints` and
  `relative-pointer` (locked and confined pointers for games and 3D programs) and
  `wlr-output-management` (kanshi, wlr-randr and wdisplays can switch outputs on and off and
  change scale, mode, rotation and position; windows leave an output that goes dark; the last
  output cannot be switched off).
- `[input]` and `[input:touchpad]` config sections: libinput settings of mice and touchpads (tap,
  tap-and-drag, natural scrolling, disable while typing, middle emulation, left handed,
  acceleration speed and profile, click, scroll and tap button map), applied when a device
  appears and on every reload.
- Tests: a `protocols` scenario (per-output preferred scale, cursor shape, activation with real
  and made up tokens, pointer lock and confinement with relative motion, output management:
  scale change, off, on, refusal of an all-off configuration) and parser tests for `[input]`.
- Plugin API (`include/sfwc-plugin.h`, docs/PLUGINS.md; the Hyprland and Wayfire style animations
  and wobbly windows are plugins in the sfwc-plugins repository): `[plugins] load = NAME` loads
  `NAME.so` from `$SFWC_PLUGIN_PATH`, the user's data directory or the install directory;
  `[plugin:NAME]` sections hold the plugin's settings. Plugins get a frame callback, a view of the
  windows, settings, and are loaded, reconfigured and unloaded live on config reload. Missing,
  refusing, wrong-version or wrong-wlroots plugins are logged and skipped. `sfwc-plugin.pc` and the
  header are installed for plugins in other repositories.
- First plugin: `wobbly` (windows wobble on a spring mesh when moved). It lives in the separate
  repository sfwc-plugins; the compositor only has the API and two test plugins.
- Tests: `[plugins]`/`[plugin:NAME]` parsing and an end-to-end scenario for the plugin API
  (`compositor-client-plugins`, with a test plugin). The `wobbly` scenario of the test client is run
  by the sfwc-plugins CI against the real plugin.

### Changed
- **The eight extra themes moved out of the compositor repository** into the theme pack of
  sfwc-plugins (nord, gruvbox-dark, dracula, tokyo-night, rose-pine, solarized-dark/light,
  high-contrast, with previews and the contrast test). Installed to the same place, they work as
  before (`theme = nord`). The compositor keeps `default` and `light` and the templates.
- **Animations are now a plugin.** The Hyprland style rules and curves, popin/slide/slidefade,
  workspace slides, the border fade, and the fire, squeeze and zoom effects moved to the
  `animations` plugin in the sfwc-plugins repository; the compositor has no animation code any
  more (windows appear, move and close at once unless the plugin is loaded). The `[animations]`
  section is passed to the plugin as it is, so existing configs keep working after
  `load = animations`. To make that possible the plugin API is now **version 2**: the plugin
  table carries its size, and there are hooks for mapped, committed, moved and cancelled windows,
  focus changes, workspace switches and layer surfaces, plus host functions for the output
  boxes, all lines of a plugin's section, the clock, a "busy" mark and the frame color mix.
  Plugins built for version 1 must be rebuilt.
- Builds against **wlroots 0.20.0** (was 0.18.2). Needs wayland >= 1.24 and xkbcommon >= 1.8,
  which meson builds from `subprojects/` when the system's are older (new `xkbcommon.wrap`;
  `bison` and `flex` are needed for that). The only API change that affected the code was the
  removal of `wlr_xdg_surface_get_geometry()`: the window geometry is now read from
  `wlr_xdg_surface.geometry`. All unit and end-to-end tests pass unchanged on 0.20.
- CI runs only sfwc's own tests (`--suite sfwc`): the wlroots subprojects bring slow tests of their own.
- wlroots is built as a shared library (plugins must share the compositor's copy).

## v0.6.0 – 2026-10-09 – M3 to M7: configuration, look and feel, desktop integration, theme templating

Everything since v0.2.0. Builds against wlroots 0.18.2. (The wlroots 0.20 port, the Hyprland and
Wayfire style animations and wobbly windows live on separate branches and are not part of this
release.) Milestones: M3 configuration, M4 look and feel, M5 desktop integration, M6 themes and
templating, M7 session integration.

### Changed
- `src/main.c` (3000 lines) is split into modules by topic (window, output, input, cursor,
  layers, lock, ...) around a shared `src/server.h`; no behavior change.

### Added
- Eight more themes (nord, gruvbox-dark, dracula, tokyo-night, rose-pine, solarized-dark,
  solarized-light, high-contrast), mock-up previews in `docs/themes/` generated from the theme
  files, and a README gallery.
- Tests: every shipped theme is checked for validity and readable contrast, the previews are
  checked for being current, and a fuzz test feeds garbage to the config/theme/template parsers.
- GitHub: issue forms, pull request template, `SECURITY.md`, a rewritten README.
- Session integration (M7): `sfwc.desktop` for display managers, man pages, Ctrl+Alt+F1..F12
  VT switching, `XDG_CURRENT_DESKTOP=SFWC`, D-Bus activation environment, install check in CI.
- Theme templating (M6): `sfwc-theme-apply` renders `@section.key@` templates (waybar, fuzzel,
  foot, mako, swaylock) into `$XDG_RUNTIME_DIR/sfwc/` on start and reload; `[templates]`
  config section; theme placeholders in `spawn:` and `[autostart]` commands.
- Workspaces (`workspaces = N`, `workspace:N`, `move-to-workspace:N`, defaults on `$mod+1..4`).
- `ext-session-lock` for screen lockers.
- `wlr-layer-shell`: panels, wallpapers and launchers (M5). Exclusive zones shrink the area used
  for maximizing, placing and snapping windows; exclusive/on-demand keyboard focus; popups.
- Primary selection, `wlr-data-control` (clipboard managers), `ext-idle-notify` and idle inhibit
  (swayidle and friends), `wlr-foreign-toplevel-management` (taskbars can list, activate,
  minimize, maximize and close windows).
- End-to-end test `compositor-client-layers`; every scenario also checks the advertised globals.
- Config file (`~/.config/sfwc/sfwc.conf` or `$SFWC_CONFIG`): gap, snapping (edges and other
  windows), focus mode, terminal, configurable keybinds and mouse binds (`$mod`, any xkb key,
  `spawn:<cmd>`), `[autostart]` commands. Errors are logged with line numbers and never stop
  the compositor.
- `[keyboard]` section (xkb layout/variant/options, repeat rate and delay) and `[output:NAME]`
  sections (scale, position, enabled).
- Live reload when the file is saved, plus the `reload-config` action (Alt+Shift+r).
- Multi-monitor: windows open on the output under the pointer, maximize/fullscreen/snapping use
  that output's logical size, `move-to-next-output` (Alt+o) and `focus-next-output`
  (Alt+Shift+o), `xdg-output` protocol for clients.
- Snapping to other windows while moving.
- `SFWC_LOG_LEVEL` (debug logs every action).
- Tests: parser/config unit tests, and three end-to-end runs (single output, two outputs with
  different scale/position, nested inside another sfwc) using virtual keyboard and pointer.

- Themes: loader with a built-in default, `format = 1`, search path, live reload. Window
  decorations drawn from the theme (title bar with text, borders, buttons, rounded corners,
  shadow) for clients that use `xdg-decoration`, with mouse interaction.
- Animations for opening, closing and moving windows (`[animations]`), `SFWC_NO_ANIMATIONS`.
- `wlr-screencopy` protocol; end-to-end tests compare screen captures pixel by pixel.

### Changed
- Keybinds match the exact modifier set and the unshifted key (`Shift+m` instead of `M`).
- Keybind actions apply to the window that has keyboard focus (was: the front window).

### Security
- Theme `name` and `[font] family` only accept letters, digits, spaces and `_-.,+`. They are
  expanded into `spawn:`/`[autostart]` shell commands, so a shared theme could otherwise run
  commands. THEMES.md now explains that themes shipping their own templates must be trusted.
- Theme placeholders in `spawn:`/`[autostart]` commands are also single-quoted for the shell
  (this also keeps `#rrggbb` from being read as a shell comment).

### Fixed
- `sfwc-theme-apply` no longer leaks memory when it runs out of memory reading a file.

## v0.2.0 – 2026-10-08 – "Usable floating WM" (alpha)

First tagged release. A small wlroots 0.18 compositor that opens, stacks, moves,
resizes, maximizes, fullscreens and minimizes windows. Not yet configurable:
keybinds and look are hard-coded (config comes in v0.3.0).

### Added
- Outputs and rendering through the wlroots scene graph; nested and headless backends.
- xdg-shell windows and popups, stacking order, click-to-focus, focus handover when a
  window closes or is minimized, cascading placement (dialogs centered).
- Maximize, fullscreen and minimize (keyboard or app request); the old size is restored.
- Alt+drag to move (left button) and resize (right button); snapping to screen edges.
- Built-in keybinds (modifier Alt): Return terminal, q close, Tab cycle, f maximize,
  F11 fullscreen, m minimize, Shift+m restore, Esc quit. See the README table.
- Cursor theme/size from `XCURSOR_THEME` / `XCURSOR_SIZE`.
- Vendored inih (r58) config parser with tests for the example config and themes.
- Example config (`config/sfwc.conf`) and optional theme package (`themes/`) with a
  templating design (docs/THEMES.md); not wired into the compositor yet.
- CI: sanitizer builds (ASan/UBSan), parser tests, and an end-to-end test that runs
  sfwc headless and drives it with a real Wayland client plus virtual keyboard and
  pointer (opt-in via `SFWC_ENABLE_VIRTUAL_INPUT=1`, testing only).

### Known limitations
- Config file, themes, window decorations, animations: not implemented yet.
- Multi-monitor is untested; windows cannot yet snap to other windows.
- Only tested on the headless backend in CI; real hardware and nested use are untested.
- wlroots 0.18.2 and wayland 1.23.1 are built from source (subprojects) when the system
  does not provide them, so the first build is slow and needs network access.
