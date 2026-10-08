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
| general | `theme` | name, `default` | theme name (themes arrive with the theme package) |
| general | `terminal` | command, `$SFWC_TERMINAL` or `foot` | used by `$terminal` |
| general | `focus` | `click` (default), `follow-mouse` | follow-mouse focuses without raising |
| general | `mod` | `Alt` (default), `Super`, `Ctrl`, `Shift` | what `$mod` means in binds; set it **before** the binds |
| windows | `gap` | 0–200, `8` | space to screen edges when placing, maximizing, snapping |
| windows | `snap_to_edges` | bool, `true` | snap moved windows to screen edges |
| windows | `snap_distance` | 0–200, `12` | px from an edge at which snapping happens |
| windows | `default_layout` | `floating` | only floating exists |
| animations | `enabled open close move resize duration_ms easing` | see `config/sfwc.conf` | validated now, used once animations land |

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
| `reload-config` | re-read the config file |
| `quit` | exit the compositor |

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

## Testing aids

`SFWC_ENABLE_VIRTUAL_INPUT=1` exposes the virtual keyboard/pointer protocols so tests can
inject input. Leave it unset: any client could otherwise type into your session.
