# Roadmap

Each milestone should leave `sfwc` runnable (nested first, TTY later). Items
are roughly in build order; check them off as they land.

## M0 – Foundation  *(done)*
- [x] Repo layout, meson build, docs, example config and themes
- [x] Vendor inih (r58, `third_party/inih`); comment rules: full-line only
- [x] Parser tests (`meson test`) covering the shipped config and themes
- [x] ASan/UBSan builds documented (CONTRIBUTING) and used in CI
- [x] CI fails on errors; wlroots 0.18 comes from `subprojects/wlroots.wrap`
- [x] `src/main.c` compiles against wlroots 0.18.2 (built from the wraps) in CI, with ASan/UBSan

## M1 – A window on screen
- [ ] Server struct, signal handling, clean shutdown
- [ ] Outputs (monitors) + `wlr_scene`, nested backend works
- [ ] xdg-shell toplevels: map/unmap, popups
- [ ] Keyboard + pointer via seat, click-to-focus, raise on focus
- [ ] Launch a terminal with a keybind (spawn with setsid + SIGCHLD reaping)

## M2 – Usable floating WM
- [ ] Interactive move and resize with modifier+mouse
- [ ] Stacking order, cycle windows, close, minimize, maximize, fullscreen
- [ ] Edge snapping and gaps, sane initial window placement
- [ ] Multi-monitor layout and per-output focus
- [ ] Cursor theme (xcursor)

## M3 – Configuration
- [ ] Config parser, defaults, error messages with line numbers
- [ ] Keybind and mouse-bind tables from config
- [ ] Live reload (inotify on config dir) and `reload-config` action
- [ ] `[autostart]` with `$theme.*` / `$runtime` expansion

## M4 – Look and feel
- [ ] Built-in fallback theme
- [ ] Theme parser; server-side decorations (xdg-decoration), title bar, buttons
- [ ] Title text with cairo + pango
- [ ] Rounded corners and shadows
- [ ] Animation engine: tweens + easing, open/close, then move/resize
- [ ] Animations fully disable-able; respect reduced-motion setting

## M5 – Desktop integration
- [ ] `wlr-layer-shell` (bars, wallpaper, launcher)
- [ ] `ext-session-lock`, `ext-idle-notify`, idle inhibit
- [ ] `wlr-foreign-toplevel-management` (taskbar), workspace protocol
- [ ] Workspaces + workspace-switch animation
- [ ] Screenshots / screen capture, clipboard and primary selection

## M6 – Themes and templating  *(separate `sfwc-themes` package)*
- [ ] Add `format = 1` to the theme format and check it on load
- [ ] `sfwc-theme-apply` helper: placeholders `@section.key@`, `:hex` modifier
- [ ] Templates for waybar, fuzzel, swaybg (colors/fonts/borders only)
- [ ] Compositor runs the helper on start and reload, signals running tools
- [ ] More tools (swaylock, mako, terminal colors) and more themes
- [ ] Move `themes/` to its own repo once format and templates are stable

## M7 – Release
- [ ] Xwayland
- [ ] TTY session: `.desktop` file for display managers, seatd/logind checks
- [ ] Man pages, packaging (Arch/AUR, Debian, Nix), tagged 0.1.0

## Later / ideas
- Optional tiling-assist (snap zones), window rules, per-app opacity
- IPC socket and a small control CLI (`sfwcctl`)
- HiDPI / fractional scaling polish, HDR, tablet/touch gestures
