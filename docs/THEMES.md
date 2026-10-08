# Themes

Themes are a separate, optional package (see [../themes/](../themes/)). Without
it, SFWC uses a built-in minimal look.

A theme is a single `.theme` file in `~/.config/sfwc/themes/` (or
`/usr/share/sfwc/themes/`). Select it with `theme = <name>` under `[general]`.

Sections: `[colors]`, `[geometry]`, `[shadow]`, `[font]`. Colors are
`#rrggbb` or `#rrggbbaa`. See [default.theme](../themes/default.theme)
and [light.theme](../themes/light.theme). Share a theme by sharing the file.

## Format reference

A theme is an ini-style file (`format = 1`), loaded by name. Search order for `theme = NAME`:
`<config dir>/themes/NAME.theme`, `~/.config/sfwc/themes`, `~/.local/share/sfwc/themes`,
`/usr/local/share/sfwc/themes`, `/usr/share/sfwc/themes`. The built-in `default` theme is
always available (it is identical to `themes/default.theme`); an unknown name is reported in
the log and the built-in theme is used. Saving the config or the theme's name changing
reloads the theme live. Mistakes are logged with their line number and keep the default.

```ini
format = 1            # required to be 1 if present
name = My Theme

[colors]              # #rrggbb or #rrggbbaa
background       = #1e1e2e
border_focused   = #89b4fa
border_unfocused = #45475a
titlebar_focused   = #313244
titlebar_unfocused = #1e1e2e
title_text       = #cdd6f4
close_button     = #f38ba8
maximize_button  = #a6e3a1
minimize_button  = #f9e2af

[geometry]            # pixels
border_width    = 2   # 0-50
titlebar_height = 28  # 0-200 (0 = no titlebar)
corner_radius   = 8   # 0-100, outer corners of the frame
button_size     = 14
button_spacing  = 8

[shadow]
enabled  = true
radius   = 20         # blur radius
offset_y = 6
color    = #00000066

[font]
family = sans
size   = 10           # points
```

How it is used:

- Windows are decorated by the compositor only when the client asks for it through the
  `xdg-decoration` protocol (foot, most GTK/Qt apps with the right setting do); other
  clients draw their own title bars and are left alone. `[windows] decorations = false`
  turns server-side decorations off for everyone.
- Layout: border on all sides, the titlebar above the content (inside the border), buttons
  right-aligned (minimize, maximize, close from left to right), the title left-aligned and
  ellipsized before the buttons. Fullscreen windows have no frame. Maximized windows keep
  theirs, and the whole frame fits inside the gap.
- Click the buttons, drag the titlebar to move, double-click it to maximize, drag the border
  or corners to resize.
- `background` is reserved for the wallpaper/background support (layer shell) and templates;
  the frame itself is drawn from the other colors.
- Rounded corners apply to the frame. The client's own content stays rectangular (wlroots 0.18
  cannot clip surfaces), so the bottom corners are rounded only as far as the border width
  allows; use a border of at least a third of the radius for fully round corners at the bottom.
- The shadow is drawn at a quarter of the resolution and blurred, so it is cheap for large windows.

## One theme for the whole desktop (templating)

A theme styles more than SFWC's own window decorations. It also drives the companion tools
(status bar, launcher, lock screen, notifications, terminal) so you maintain one file instead
of one per tool.

How it works:

1. The theme package ships **templates** in `templates/`, named after the output file plus
   `.in` (`waybar.css.in`, `fuzzel.ini.in`, `foot.ini.in`, `mako.conf.in`,
   `swaylock.conf.in`).
2. Templates contain placeholders `@section.key@` for theme keys, e.g.
   `@colors.border_focused@`, `@geometry.border_width@`, `@font.family@`, `@theme.name@`.
   Colors print as `#rrggbb` (`#rrggbbaa` when not opaque); a modifier picks another format:
   `@colors.border_focused:hex@` = `89b4fa`, `:hexa` = `89b4faff`, `:rgb` = `137, 180, 250`,
   `:rgba` = `rgba(137, 180, 250, 1.00)`. `@@` is a literal `@`. An unknown key, an unknown
   modifier or a lone `@` is an error that names the file and line; the other templates are
   still rendered.
3. The helper **`sfwc-theme-apply`** (part of the compositor package) renders every template
   into `$XDG_RUNTIME_DIR/sfwc/` (never `/tmp`), e.g. `$XDG_RUNTIME_DIR/sfwc/waybar.css`, and
   only rewrites files whose content changed. Template directories, first match wins:
   `<config dir>/themes/<theme>/templates`, `<config dir>/templates`,
   `~/.config/sfwc/templates`, `~/.local/share/sfwc/templates`,
   `/usr/local/share/sfwc/templates`, `/usr/share/sfwc/templates` (so you can override a
   packaged template, and a theme can bring its own). `SFWC_TEMPLATES=dir[:dir]` replaces the
   list (development).
4. The compositor runs the helper on startup and on every config reload, before it starts the
   `[autostart]` programs. Waybar is sent `SIGUSR2` and mako `makoctl reload` when their output
   changed; other tools pick the files up on their next start.

Using the output:

| Tool | Output | How to use it |
|---|---|---|
| waybar | `waybar.css` | `@import url("/run/user/1000/sfwc/waybar.css");` in `style.css` |
| fuzzel | `fuzzel.ini` | `fuzzel --config $XDG_RUNTIME_DIR/sfwc/fuzzel.ini` |
| foot | `foot.ini` | `include=/run/user/1000/sfwc/foot.ini` in `foot.ini` |
| mako | `mako.conf` | `mako --config $XDG_RUNTIME_DIR/sfwc/mako.conf` |
| swaylock | `swaylock.conf` | `swaylock --config $XDG_RUNTIME_DIR/sfwc/swaylock.conf` |
| swaybg | (no file) | `exec = swaybg -c @colors.background:hex@` |

The last row shows that the same `@section.key@` placeholders also work in `spawn:` binds and
`[autostart]` commands (only placeholders that name a theme key are replaced, so `ssh me@host`
is safe). Switch templates off with `[templates]` in the config (`enabled = false`, or
`waybar = false` for one of them).

Why templates and not C code in the compositor:

- The compositor stays small and does not need to know any tool's file format.
- Supporting a new tool = adding a template, no recompiling.
- A theme can ship its own templates, so it can restyle more than colors.
- Tools you don't use, or configure yourself, are simply skipped.

Scope: colors, fonts, border width and corner radius. Layout and module configuration of the
tools stays in the user's own config files. Tool config formats change over time, so templates
are versioned together with the theme package.

Config parsing rule: comments are full-line only (`#` or `;` at line start). Inline comments
are not supported, so `#rrggbb` values need no escaping.
