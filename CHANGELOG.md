# Changelog

Versions follow the roadmap milestones while the project is pre-1.0:
`0.<milestone>.0` (v0.2.0 = M2). Dates are UTC.

## Unreleased – M3 "Configuration"

### Added
- Config file (`~/.config/sfwc/sfwc.conf` or `$SFWC_CONFIG`): gap, snapping, focus mode,
  terminal, configurable keybinds and mouse binds (`$mod`, any xkb key, `spawn:<cmd>`),
  `[autostart]` commands. Errors are logged with line numbers and never stop the compositor.
- Live reload when the file is saved, plus the `reload-config` action (Alt+Shift+r).
- `src/config.c` parser with unit tests; end-to-end test covers reload and autostart.

### Changed
- Keybinds match the exact modifier set and the unshifted key (`Shift+m` instead of `M`).

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
