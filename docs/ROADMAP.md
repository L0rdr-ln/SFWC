# Roadmap

Each milestone should leave `sfwc` runnable (nested first, TTY later). Items
are roughly in build order; check them off as they land.

## M0 – Foundation  *(done)*
- [x] Repo layout, meson build, docs, example config and themes
- [x] Vendor inih (r58, `third_party/inih`); comment rules: full-line only
- [x] Parser tests (`meson test`) covering the shipped config and themes
- [x] ASan/UBSan builds documented (CONTRIBUTING) and used in CI
- [x] CI fails on errors; wlroots comes from `subprojects/wlroots.wrap` (0.20.0)
- [x] the compositor builds against wlroots 0.20.0 (built from the wraps) in CI, with ASan/UBSan

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

## M4 – Look and feel  *(done; checked pixel by pixel in CI with screen captures)*
- [x] Built-in fallback theme (identical to `themes/default.theme`, checked by a test)
- [x] Theme parser (`src/theme.c`): `format = 1`, validation, line numbers, search path,
      live reload
- [x] Server-side decorations via `xdg-decoration`: borders, titlebar, minimize/maximize/close
      buttons, drag to move, border/corner drag to resize, double-click to maximize
- [x] Title text with cairo + pango
- [x] Rounded corners and blurred shadows (on the frame; client content stays rectangular)
- [x] Animation engine (`src/anim.c`): easing, time-based progress; open, close and move
      animations, driven by the output frame callbacks
- [x] Animations can be disabled (config, `SFWC_NO_ANIMATIONS=1`) and are switched off live
- [x] `wlr-screencopy` exposed (also what the tests use to look at the screen)
- Not possible with wlroots 0.18: scaling windows (so `fade-scale` is a fade + slide) and
  clipping client content to rounded corners; resize animations would need the client to cooperate

## M5 – Desktop integration
- [x] `wlr-layer-shell` (bars, wallpaper, launcher; exclusive zones, exclusive keyboard focus, popups)
- [x] `ext-idle-notify`, idle inhibit
- [x] `ext-session-lock`
- [x] `wlr-foreign-toplevel-management` (taskbar)
- [x] Workspaces: `workspace:N` / `move-to-workspace:N`, per-window workspace
- [x] Workspace-switch animation (slide, slidevert, slidefade, fade)
- [ ] `ext-workspace` protocol (wlroots 0.20 has it; needs the 0.20 branch)
- [x] Screenshots / screen capture (screencopy), clipboard, primary selection, data-control

## M6 – Themes and templating  *(separate `sfwc-themes` package)*
- [x] Add `format = 1` to the theme format and check it on load
- [x] `sfwc-theme-apply` helper: placeholders `@section.key@`, `:hex`/`:hexa`/`:rgb`/`:rgba` modifiers
- [x] Templates for waybar, fuzzel (swaybg via placeholders in the command)
- [x] Compositor runs the helper on start and reload, signals running tools
- [x] Expand theme values (`@section.key@`) in `[autostart]` and `spawn:` commands
- [x] More tools: swaylock, mako, foot
- [x] More themes: nord, gruvbox-dark, dracula, tokyo-night, rose-pine, solarized-dark/light,
      high-contrast (checked for readable contrast by a test)
- [ ] Move `themes/` to its own repo once format and templates are stable

## M7 – Release
- [ ] Xwayland: deliberately **after 1.0**. It needs a second window kind next to xdg-shell
      (X11 override-redirect windows, ICCCM state) and the X libraries in every package.
- [x] TTY session: `sfwc.desktop` for display managers, `XDG_CURRENT_DESKTOP`/`XDG_SESSION_TYPE`,
      D-Bus activation environment, Ctrl+Alt+F1..F12 VT switching (libseat)
- [x] Man pages (`sfwc.1`, `sfwc.conf.5`, `sfwc-theme-apply.1`), `meson install` checked in CI
- [ ] Test on real hardware (TTY, several GPUs, multi-monitor); needs a person with a machine.
      `tools/try-sfwc.sh` and `docs/HARDWARE-TESTING.md` make it a five minute job.
- [ ] Packaging (Arch/AUR, Debian, Nix), tag 1.0.0 (tags/releases are made by the maintainer)

## Later / ideas
- Optional tiling-assist (snap zones), window rules, per-app opacity
- IPC socket and a small control CLI (`sfwcctl`)
- HiDPI / fractional scaling polish, HDR, tablet/touch gestures
