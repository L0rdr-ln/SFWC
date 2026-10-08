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

## M1 – A window on screen  *(done; tested in CI headless and nested)*
- [x] Server struct, signal handling (SIGINT/SIGTERM), clean shutdown
- [x] Outputs (monitors) + `wlr_scene`
- [x] xdg-shell toplevels and popups
- [x] Keyboard + pointer via seat, click-to-focus, raise on focus
- [x] Spawn a terminal with Alt+Return (double fork, no zombies); the configured terminal
      is started in the end-to-end test
- [x] Bonus: Alt+drag move/resize, Alt+Tab, Alt+q, Alt+Esc
- [x] Automated client test in CI: toplevel + popup + frame callback, clean shutdown
- [x] Input-driven behavior in CI via virtual keyboard/pointer (see M2)
- [x] Nested use in CI: `sfwc` with the wayland backend as a window of another `sfwc`
- [ ] Real hardware (DRM/KMS output, libinput devices) and the system cursor theme can not
      be exercised in CI; try it on a TTY and report problems

## M2 – Usable floating WM  *(done; window state, input and multi-monitor tested in CI)*
- [x] Interactive move and resize with modifier+mouse
- [x] Stacking order, cycle windows (Alt+Tab), close (Alt+q)
- [x] Maximize (Alt+f), fullscreen (Alt+F11), minimize (Alt+m / restore Alt+Shift+m);
      client requests are honored; saved size is restored
- [x] Edge snapping while moving and a configurable gap
- [x] Snapping to other windows (keeps the gap between them)
- [x] Sane initial placement: cascading, dialogs centered, clamped to the output
- [x] Focus moves to the next window when one closes or is minimized
- [x] Cursor theme/size from `XCURSOR_THEME` / `XCURSOR_SIZE`
- [x] Multi-monitor: new windows open on the output under the pointer; maximize, fullscreen,
      snapping and placement use that output's logical (scaled) size; `move-to-next-output`
      and `focus-next-output` actions; layout via per-output position and scale;
      `xdg-output` for clients. Tested with two outputs of different size and scale.
- [x] Actions apply to the keyboard-focused window (matters for `focus = follow-mouse`)

## M3 – Configuration  *(done; unit tests plus end-to-end tests)*
- [x] Config parser (`src/config.c`, independent of wlroots): defaults, validation,
      error messages with line numbers, unit tests incl. the shipped example config
- [x] Keybind and mouse-bind tables from config (`$mod`, modifiers, any xkb key name,
      `spawn:<cmd>` and the built-in actions); last definition wins; sections replace defaults
- [x] `gap`, `snap_distance`, `snap_to_edges`, `snap_to_windows`, `focus`
      (click | follow-mouse), `terminal` are config; the old hard-coded constants are gone
- [x] Live reload (inotify on the config directory) and the `reload-config` action, both
      tested; unreadable file keeps the old config; maximized windows are re-fitted
- [x] `[autostart]` with `$terminal`, `$theme`, `$runtime` expansion
- [x] `[keyboard]`: xkb layout/variant/options/model/rules (invalid layouts fall back and are
      reported) and repeat rate/delay, re-applied on reload
- [x] `[output:NAME]`: scale, position, enabled, applied at start and on reload
- [x] `focus = follow-mouse` tested
- Moved to M6 (needs themes): expanding individual theme values (`$theme.<key>`) in commands

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
- [ ] Expand theme values (`$theme.<key>`) in `[autostart]` commands
- [ ] More tools (swaylock, mako, terminal colors) and more themes
- [ ] Move `themes/` to its own repo once format and templates are stable

## M7 – Release
- [ ] Xwayland
- [ ] TTY session: `.desktop` file for display managers, seatd/logind checks
- [ ] Man pages, packaging (Arch/AUR, Debian, Nix), tagged 1.0.0

## Later / ideas
- Optional tiling-assist (snap zones), window rules, per-app opacity
- IPC socket and a small control CLI (`sfwcctl`)
- HiDPI / fractional scaling polish, HDR, tablet/touch gestures
