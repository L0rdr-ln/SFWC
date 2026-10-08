# sfwc-themes

Optional theme package for SFWC. The compositor works without it using a
built-in minimal look; install this package only if you want extra themes.

```sh
meson setup build-themes themes
meson install -C build-themes     # installs to <prefix>/share/sfwc/themes
```

Or copy a `.theme` file into `~/.config/sfwc/themes/`. See
[../docs/THEMES.md](../docs/THEMES.md) for the format.
