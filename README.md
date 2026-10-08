# Simple Floating Wayland Compositor (SFWC)

A small, easy-to-configure **floating (stacking) Wayland compositor** built on
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), with themeable
window decorations and animations.

> **Status: early skeleton.** The project layout, build system, config format
> and roadmap are in place; the compositor itself is being built up step by
> step (see [docs/ROADMAP.md](docs/ROADMAP.md)).

## Goals

- **Floating/stacking first** – windows overlap, can be moved, resized,
  raised, minimized and maximized like in classic desktop environments.
- **Simple configuration** – one plain-text file, sensible defaults, live
  reload. No scripting language required.
- **Themes** – colors, borders, title bars, corner radius, shadows and fonts
  defined in small theme files you can swap or share. Themes ship as a
  separate optional package ([`themes/`](themes/)); the compositor runs fine
  with its built-in minimal look. With the package installed, one theme can
  also style your bar and launcher through templates.
- **Animations** – configurable open/close/move/resize/workspace animations
  with adjustable duration and easing, or turn them off entirely.
- **Small and readable** – written in C against wlroots; easy to hack on.

## Build

Dependencies (names are for Debian/Ubuntu; check your distro):

| Dependency | Package |
|---|---|
| wlroots 0.18 | `libwlroots-0.18-dev` where available; otherwise meson builds it automatically from `subprojects/wlroots.wrap` |
| wayland-server, wayland-protocols | `libwayland-dev wayland-protocols` |
| xkbcommon | `libxkbcommon-dev` |
| pixman | `libpixman-1-dev` |
| meson, ninja, pkg-config, C compiler | `meson ninja-build pkg-config gcc` |

```sh
meson setup build
meson compile -C build
meson test -C build
./build/sfwc            # run from a TTY, or nested inside another compositor
```

Running nested (easiest for development): start `sfwc` from a terminal inside
an existing Wayland session and it opens in a window.

## Default keybindings

These are the built-in defaults (modifier is Alt so it works when nested); all of them can be
changed in the config, see [docs/CONFIGURATION.md](docs/CONFIGURATION.md):

| Keys | Action |
|---|---|
| Alt+Return | open terminal (`$SFWC_TERMINAL`, default `foot`) |
| Alt+q | close focused window |
| Alt+Tab | cycle windows |
| Alt+f / Alt+F11 | toggle maximize / fullscreen |
| Alt+m / Alt+Shift+m | minimize / restore last minimized |
| Alt+drag (left / right button) | move / resize window |
| Alt+Shift+r | reload the config |
| Alt+Esc | quit |

## Configuration

Copy the example config and edit it:

```sh
mkdir -p ~/.config/sfwc
cp config/sfwc.conf ~/.config/sfwc/sfwc.conf   # optional; saving the file reloads it live
```

Themes are optional and installed separately, see [themes/README.md](themes/README.md).

See [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for all options and
[docs/THEMES.md](docs/THEMES.md) for writing themes.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Configuration](docs/CONFIGURATION.md)
- [Themes](docs/THEMES.md)
- [Roadmap](docs/ROADMAP.md)
- [Contributing](CONTRIBUTING.md)

## License

MIT – see [LICENSE](LICENSE).
