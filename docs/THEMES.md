# Themes

Themes are a separate, optional package (see [../themes/](../themes/)). Without
it, SFWC uses a built-in minimal look.

A theme is a single `.theme` file in `~/.config/sfwc/themes/` (or
`/usr/share/sfwc/themes/`). Select it with `theme = <name>` under `[general]`.

Sections: `[colors]`, `[geometry]`, `[shadow]`, `[font]`. Colors are
`#rrggbb` or `#rrggbbaa`. See [default.theme](../themes/default.theme)
and [light.theme](../themes/light.theme). Share a theme by sharing the file.

## One theme for the whole desktop (templating)

A theme styles more than SFWC's own window decorations. It can also drive the
companion tools (status bar, launcher, lock screen, ...) so you maintain one
file instead of one per tool.

How it works:

1. The theme package ships **templates** in `templates/`, named after the
   output file plus `.in` (for example `waybar.css.in`, `fuzzel.ini.in`).
2. Templates contain placeholders that refer to theme keys as
   `@section.key@`, e.g. `@colors.border_focused@`, `@font.family@`.
   A modifier selects a format: `@colors.border_focused:hex@` gives `89b4fa`
   (no `#`), for tools that want bare hex. Unknown placeholders are an error.
3. The helper **`sfwc-theme-apply`** renders every template of the active
   theme into `$XDG_RUNTIME_DIR/sfwc/` (never `/tmp`), e.g.
   `$XDG_RUNTIME_DIR/sfwc/waybar.css`.
4. SFWC runs the helper on startup and on `reload-config`, then tells running
   tools to pick up changes (Waybar: `SIGUSR2`; swaybg, fuzzel: respawn/next
   launch).

Why templates and not C code in the compositor:

- The compositor stays small and does not need to know any tool's file format.
- Supporting a new tool = adding a template, no recompiling.
- A theme can ship its own templates, so it can restyle more than colors.
- Tools you don't use, or configure yourself, are simply skipped (`[templates]`
  in the config can enable/disable each one).

Scope for the first version: colors, fonts, border width and corner radius for
**waybar, fuzzel and swaybg**. Layout and module configuration of the tools
stays in the user's own config files. Tool config formats change over time, so
templates are versioned together with the theme package.

Config parsing rule: comments are full-line only (`#` or `;` at line start).
Inline comments are not supported, so `#rrggbb` values need no escaping.
