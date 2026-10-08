# sfwc-themes

Optional theme package for SFWC. The compositor works without it using a
built-in minimal look; install this package only if you want extra themes.

```sh
meson setup build-themes themes
meson install -C build-themes     # installs to <prefix>/share/sfwc/themes
```

Included: `default`, `light`, `nord`, `gruvbox-dark`, `dracula`, `tokyo-night`, `rose-pine`,
`solarized-dark`, `solarized-light` and `high-contrast` (previews in the
[main README](../README.md#themes)). Switch with `theme = <name>` in `sfwc.conf`.

Or copy a `.theme` file into `~/.config/sfwc/themes/`. See
[../docs/THEMES.md](../docs/THEMES.md) for the format.

This package also ships `templates/` (see the templating section in
[../docs/THEMES.md](../docs/THEMES.md)) so one theme can style waybar, fuzzel
and swaybg too.
