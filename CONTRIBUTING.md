# Contributing

1. Install the dependencies listed in the [README](README.md#build).
2. `meson setup build && meson compile -C build`
   (uses system wlroots 0.18 if installed, otherwise builds it from
   `subprojects/wlroots.wrap`; needs git and network)
   - Only the parser and tests, no wlroots: `meson setup build -Dcompositor=false`
   - Sanitizer build: `meson setup build -Db_sanitize=address,undefined -Db_lundef=false`
   - Tests: `meson test -C build --print-errorlogs` (includes `compositor-client`,
     which starts sfwc headless and connects a real Wayland client to it)
3. Run nested inside an existing Wayland session: `./build/sfwc`
4. Format with `clang-format -i src/*.c src/*.h` (config in `.clang-format`).
5. Keep changes small and focused; one feature per pull request.

Code style: C11, 4-space indent, Linux-style braces, no global state where a
struct passed around will do. Prefer wlroots' scene graph API for rendering.

## Tests

`meson test -C build` runs (sanitizer build in CI):

| Test | What it covers |
|---|---|
| `config-parser`, `theme-parser`, `config-and-theme-parsing` | every option, limits, error messages with line numbers, the shipped config and default theme |
| `template-rendering`, `theme-apply-helper`, `shipped-templates` | `@section.key@` rendering (strict, lenient, shell-quoted), the `sfwc-theme-apply` helper, every shipped template with every shipped theme |
| `shipped-themes` | every `themes/*.theme`: loads without messages, readable contrast (title text 4.5:1, buttons 1.8:1), unique name, installed, has a preview and a README entry |
| `theme-previews-current` | `docs/themes/*.svg` match the theme files (`tools/theme-preview.py`) |
| `parser-fuzz` | random and mutated input through the config, theme and template code under ASan/UBSan |
| `decoration-rendering`, `animation-math` | frame geometry/hit testing/drawing, easing and progress |
| `compositor-client-*` | end-to-end: `tests/run_client_test.sh` starts sfwc headless and `tests/client_test.c` talks to it as a real Wayland client |

The end-to-end modes: `single` (windows, popups, maximize/fullscreen/minimize, virtual keyboard and
pointer, snapping, terminal, key repeat, `xdg-output`, live config reload, autostart, theme
templates), `multi` (two outputs with different size/scale/position, follow-mouse, output actions),
`nested` (sfwc as a window of another sfwc), `deco` (decorations checked pixel by pixel through
screencopy, dragging, resizing, live theme reload), `anim` (open/close/move animations),
`layers` (wlr-layer-shell), `workspaces`, `lock` (ext-session-lock).

Input is injected with the wlroots virtual keyboard/pointer protocols. Those let any client
inject input, so sfwc only exposes them when started with `SFWC_ENABLE_VIRTUAL_INPUT=1`
(the script sets it). The protocol XML files come from the wlroots subproject; with a system
wlroots the input part is skipped (meson prints a warning). On failure the script prints the
compositor's log, which runs at `SFWC_LOG_LEVEL=debug`.

A test that has never failed proves little: when adding a check, break the expectation once
and confirm it goes red.

## Adding a theme

1. Copy a similar file in `themes/` (lower-case name with dashes, `format = 1`).
2. Add it to `themes/meson.build`, run `tools/theme-preview.py`, and add the preview to the
   gallery in `README.md`.
3. `meson test -C build shipped-themes` tells you if the colors are too close to read.

## Releases

1. Move the finished milestone's entries into a new `## vX.Y.0 – date – title` section
   of `CHANGELOG.md` and set `version:` in `meson.build` (and `themes/meson.build`).
2. Merge/push, wait for CI to be green on that commit.
3. `git tag -a vX.Y.0 -m "SFWC vX.Y.0" && git push origin vX.Y.0`
   The `release` workflow checks that tag and `meson.build` agree, then publishes a
   (pre-)release with the changelog section and a source tarball + sha256.
