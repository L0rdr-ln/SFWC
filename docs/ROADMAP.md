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

## M1 – A window on screen  *(implemented; CI runs a real client against it)*
- [x] Server struct, signal handling (SIGINT/SIGTERM), clean shutdown
- [x] Outputs (monitors) + `wlr_scene`
- [x] xdg-shell toplevels and popups
- [x] Keyboard + pointer via seat, click-to-focus, raise on focus
- [x] Spawn a terminal with Alt+Return (double fork, no zombies)
- [x] Bonus: Alt+drag move/resize, Alt+Tab, Alt+q, Alt+Esc
- [x] Input-driven behavior is covered in CI via virtual input (see M2).
- [ ] Still worth a manual look on real hardware / nested (`./build/sfwc` inside
      another Wayland session, then `WAYLAND_DISPLAY=... foot`): real devices,
      cursor theme, Alt+Return terminal.
- [x] Automated client test in CI (`tests/client_test.c`: toplevel + popup + frame
      callback against headless sfwc; checks the log and clean shutdown)

## M2 – Usable floating WM  *(released as v0.2.0; window state and input are tested in CI)*
- [x] Interactive move and resize with modifier+mouse (done in M1)
- [x] Stacking order, cycle windows (Alt+Tab), close (Alt+q)
- [x] Maximize (Alt+f), fullscreen (Alt+F11), minimize (Alt+m / restore Alt+Shift+m);
      client requests are honored; saved size is restored. Tested in CI.
- [x] Edge snapping while moving (to the output edge + gap) and constant gap
      (`GAP`/`SNAP_DISTANCE` in `src/main.c`, become config in M3)
- [x] Sane initial placement: cascading, dialogs centered, clamped to the output
- [x] Focus moves to the next window when one closes or is minimized
- [x] Cursor theme/size from `XCURSOR_THEME` / `XCURSOR_SIZE`
- [ ] Multi-monitor: placement, maximize and snapping already use the output
      under the window/cursor, but this is untested with more than one output;
      per-output focus and moving windows between outputs still to do
- [ ] Snap to other windows (not only screen edges)
- [x] Input is tested in CI with virtual keyboard/pointer: key passthrough, Alt+f / Alt+F11,
      click passthrough, Alt+drag move, snapping, Alt+right-drag resize, Alt+m /
      Alt+Shift+m with keyboard focus handover, Alt+q close (`tests/client_test.c`)

## M3 – Configuration  *(implemented; tested by unit tests and the end-to-end test)*
- [x] Config parser (`src/config.c`, independent of wlroots): defaults, validation,
      error messages with line numbers, unit tests incl. the shipped example config
- [x] Keybind and mouse-bind tables from config (`$mod`, modifiers, any xkb key name,
      `spawn:<cmd>` and the built-in actions); last definition wins; sections replace defaults
- [x] `gap`, `snap_distance`, `snap_to_edges`, `focus` (click | follow-mouse), `terminal`
      are config; the old hard-coded constants are gone
- [x] Live reload (inotify on the config directory) and the `reload-config` action;
      unreadable file keeps the old config; maximized windows are re-fitted
- [x] `[autostart]` with `$terminal`, `$theme`, `$runtime` expansion
- [ ] Expanding individual theme values (`$theme.<key>`) moves to M6 (templating)
- [ ] Test `focus = follow-mouse` and `reload-config` via key in the end-to-end test
- [ ] Keyboard layout / repeat rate and per-output settings in the config

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
- [ ] Man pages, packaging (Arch/AUR, Debian, Nix), tagged 1.0.0

## Later / ideas
- Optional tiling-assist (snap zones), window rules, per-app opacity
- IPC socket and a small control CLI (`sfwcctl`)
- HiDPI / fractional scaling polish, HDR, tablet/touch gestures
