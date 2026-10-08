# Configuration

Config is searched in this order: `$SFWC_CONFIG`,
`$XDG_CONFIG_HOME/sfwc/sfwc.conf`, `~/.config/sfwc/sfwc.conf`,
`/usr/share/sfwc/sfwc.conf`.

Syntax: `[section]` headers, `key = value`, `#` comments. See
[config/sfwc.conf](../config/sfwc.conf) for a fully commented example.

| Section | Key | Meaning |
|---|---|---|
| general | theme | theme name (file `themes/<name>.theme`) |
| general | focus | `click` or `follow-mouse` |
| windows | snap_to_edges, snap_distance, gap | edge snapping behavior |
| animations | enabled, open, close, move, resize, duration_ms, easing | animation behavior |
| keybinds | `Mod+Key = action` | actions: `spawn:<cmd>`, `close`, `toggle-maximize`, `minimize`, `cycle-windows`, `reload-config`, `quit` |
| mouse | `Mod+Button = move\|resize` | drag bindings |

> Not yet implemented – this documents the target format.
