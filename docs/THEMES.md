# Themes

Themes are a separate, optional package (see [../themes/](../themes/)). Without
it, SFWC uses a built-in minimal look.

A theme is a single `.theme` file in `~/.config/sfwc/themes/` (or
`/usr/share/sfwc/themes/`). Select it with `theme = <name>` under `[general]`.

Sections: `[colors]`, `[geometry]`, `[shadow]`, `[font]`. Colors are
`#rrggbb` or `#rrggbbaa`. See [default.theme](../themes/default.theme)
and [light.theme](../themes/light.theme). Share a theme by sharing the file.
