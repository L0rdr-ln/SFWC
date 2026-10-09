# sfwc-themes

Optional basic theme package for SFWC (`default` and `light`, plus the templates). The compositor
works without it using a built-in look that equals `default`. More themes are a separate theme
pack in the [sfwc-plugins](https://github.com/L0rdr-ln/sfwc-plugins) repository.

```sh
meson setup build-themes themes
meson install -C build-themes     # installs to <prefix>/share/sfwc/themes
```

Included: `default` and `light` (previews in the
[main README](../README.md#themes)). Switch with `theme = <name>` in `sfwc.conf`.

Or copy a `.theme` file into `~/.config/sfwc/themes/`. See
[../docs/THEMES.md](../docs/THEMES.md) for the format.

This package also ships `templates/` (see the templating section in
[../docs/THEMES.md](../docs/THEMES.md)) so one theme can style waybar, fuzzel
and swaybg too.
