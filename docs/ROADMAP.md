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
- [ ] **Still to verify by hand**: input-driven behavior (focus, Alt+drag
      move/resize, Alt+keybinds) is not covered by CI because headless has no
      input devices. Run nested (`./build/sfwc` inside another Wayland session,
      then `WAYLAND_DISPLAY=... foot`) and try them.
- [ ] Extend the client test with synthetic input (wlroots virtual pointer/keyboard)
- [x] Automated client test in CI (`tests/client_test.c`: toplevel + popup + frame
      callback against headless sfwc; checks the log and clean shutdown)

## M2 – Usable floating WM  *(mostly done; window state is tested in CI)*
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
- [ ] Verify by hand (no input in headless CI): Alt+drag snapping, Alt+keybinds
- [ ] Extend the client test with synthetic input (virtual pointer/keyboard)

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
