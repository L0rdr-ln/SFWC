# Configuration

SFWC reads one plain-text file. It is optional: without it the built-in defaults apply.

**Location** (first match wins): `$SFWC_CONFIG`, `$XDG_CONFIG_HOME/sfwc/sfwc.conf`,
`~/.config/sfwc/sfwc.conf`.

```sh
mkdir -p ~/.config/sfwc && cp config/sfwc.conf ~/.config/sfwc/
```

**Live reload:** saving the file applies it immediately (the directory is watched, so
editors that save via rename work). `reload-config` (default `Alt+Shift+r`) does the
same by hand. If the file cannot be read the old configuration stays active.

**Mistakes** never stop the compositor. Each problem is logged with its line number
(`config /path/sfwc.conf:12: gap: 'lots' is not a number between 0 and 200`); the
setting keeps its default and everything else still applies.

## Syntax

`[section]` headers, `key = value`, comments on their own line starting with `#` or `;`.
Inline comments are **not** supported, so `#rrggbb` values and commands containing `#`
are safe. Booleans: `true/false/yes/no/on/off/1/0`.

## Settings

| Section | Key | Values / default | Meaning |
|---|---|---|---|
| general | `workspaces` | 1-9, `4` | number of workspaces; every window lives on exactly one, new windows open on the current one |
| general | `theme` | name, `default` | theme name (themes arrive with the theme package) |
| general | `terminal` | command, `$SFWC_TERMINAL` or `foot` | used by `$terminal` |
| general | `focus` | `click` (default), `follow-mouse` | follow-mouse focuses without raising |
| general | `mod` | `Alt` (default), `Super`, `Ctrl`, `Shift` | what `$mod` means in binds; set it **before** the binds |
| windows | `gap` | 0–200, `8` | space to screen edges when placing, maximizing, snapping |
| windows | `decorations` | bool, `true` | draw title bars/borders (from the theme) for clients that ask for server-side decorations; `false` makes every client draw its own |
| windows | `snap_to_edges` | bool, `true` | snap moved windows to screen edges |
| windows | `snap_to_windows` | bool, `true` | also snap to other windows' edges, keeping `gap` between them |
| windows | `snap_distance` | 0–200, `12` | px from an edge or window at which snapping happens |
| keyboard | `rules model layout variant options` | xkb names, system default | keyboard layout, e.g. `layout = de`, `options = caps:escape`; an invalid layout is reported and the default is used |
| keyboard | `repeat_rate` / `repeat_delay` | 0–1000 / 0–10000, `25` / `600` | key repeat (characters per second, ms before repeating; rate 0 = off) |
| windows | `default_layout` | `floating` | only floating exists |
| animations | `enabled` | bool, `true` | `false` turns every animation off (reduced motion); `SFWC_NO_ANIMATIONS=1` does the same from the environment |
| animations | `open` / `close` | `none`, `fade`, `fade-scale`, `slide` (`fade`, `fade`) | `fade-scale` is a fade with a short upward slide (10 px) and `slide` a fade with a 32 px slide, because the wlroots 0.18 scene graph cannot scale windows |
| animations | `move` | bool, `true` | slide the window when it is moved by maximize, restore or `move-to-next-output` (dragging is always immediate) |
| animations | `resize` | bool | accepted for compatibility, no effect: a window's content is resized by the client |
| animations | `duration_ms` / `easing` | 0-5000 / `linear`, `ease-in`, `ease-out`, `ease-in-out` | duration and curve of all animations |

## Keybinds

```ini
[keybinds]
$mod+Return = spawn:$terminal
$mod+Shift+m = restore-minimized
```

- Left side: modifiers joined with `+`, then the key. Modifiers: `$mod`, `Alt`, `Super`,
  `Ctrl`, `Shift`. Keys are xkb keysym names (`Return`, `Tab`, `Escape`, `F11`, `q`,
  `comma`, ...). Write the **unshifted** key and add `Shift` yourself (`Shift+m`, not `M`).
- Modifiers must match exactly (Caps/Num Lock are ignored). If a combination appears twice,
  the last line wins.
- Having a `[keybinds]` section **replaces all built-in binds**: list every one you want.
  The same goes for `[mouse]` and `[autostart]`.

| Action | Effect |
|---|---|
| `spawn:<command>` | run a command via `/bin/sh -c` (`$terminal`, `$theme`, `$runtime`, `$$` are expanded) |
| `close` | ask the focused window to close |
| `toggle-maximize` / `toggle-fullscreen` | toggle for the focused window |
| `minimize` / `restore-minimized` | hide the focused window / bring back the last hidden one |
| `cycle-windows` | focus and raise the bottom-most visible window |
| `move-to-next-output` | send the focused window to the next output (left to right, then top to bottom, wrapping); maximized/fullscreen windows are re-fitted |
| `focus-next-output` | move the pointer to the middle of the next output and focus its top window |
| `workspace:<1-9>` | switch to that workspace (default `$mod+1` … `$mod+4`) |
| `move-to-workspace:<1-9>` | send the focused window to that workspace (default `$mod+Shift+1` … `$mod+Shift+4`) |
| `reload-config` | re-read the config file |
| `quit` | exit the compositor |

## Outputs (monitors)

```ini
[output:HDMI-A-1]
scale = 1.5
position = 1920,0
```

`NAME` is the name the compositor logs when the output appears (`output HDMI-A-1 added`).
`scale` (0.25-10) and `position` (`x,y` in layout pixels; without it outputs are placed
left to right) are applied on start and on reload. `enabled = false` switches an output off
(read when the output appears). Maximizing, snapping and the "next output" actions use the
output's *logical* size, i.e. the mode divided by the scale. New windows open on the output
under the pointer. Clients can read the layout through `xdg-output`.

## Mouse

```ini
[mouse]
$mod+Left = move
$mod+Right = resize
```

Hold the modifier and drag with `Left`, `Right` or `Middle`. Actions: `move`, `resize`.
The click is not passed to the window.

## Autostart

```ini
[autostart]
exec = swaybg -c 1e1e2e
exec = waybar
```

Commands run once when the compositor starts (not on reload), after it is ready to accept
clients, so they connect to it. Expansions: `$terminal`, `$theme` (theme name), `$runtime`
(`$XDG_RUNTIME_DIR`).
Expanding individual theme *values* (colors, fonts) belongs to the theme templating work,
see [THEMES.md](THEMES.md).

## Environment variables

| Variable | Meaning |
|---|---|
| `SFWC_CONFIG` | path of the config file |
| `SFWC_TERMINAL` | default for `terminal` |
| `SFWC_LOG_LEVEL` | `debug`, `info` (default), `error`, `silent`; debug logs every action |
| `XCURSOR_THEME`, `XCURSOR_SIZE` | cursor theme and size |

Testing aids (used by the test suite; leave them unset):

| Variable | Meaning |
|---|---|
| `SFWC_ENABLE_VIRTUAL_INPUT=1` | expose the virtual keyboard/pointer protocols so tests can inject input; any client could otherwise type into your session |
| `SFWC_TEST_OUTPUTS=1024x600,...` | add extra headless outputs (only with `WLR_BACKENDS=headless`) |
| `SFWC_NO_CONFIG_WATCH=1` | do not watch the config file; only `reload-config` reloads |

## Desktop integration

Bars, wallpapers and launchers use `wlr-layer-shell` (waybar, swaybg, fuzzel, ...). A bar's
exclusive zone is subtracted from the area used for maximizing, placing and snapping windows.
Other protocols offered: `ext-session-lock` (swaylock: while locked, windows are hidden, only
the lock client gets keyboard and pointer, and all keybinds except `quit` are off; a locker
that crashes leaves the session locked), `ext-idle-notify` and idle inhibit (swayidle),
`wlr-foreign-toplevel-management` (taskbars), primary selection, `wlr-data-control` (clipboard
managers) and `wlr-screencopy` (grim).
