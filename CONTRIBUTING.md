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

`meson test -C build` runs, besides the parser/config unit tests, three end-to-end runs of
`tests/run_client_test.sh` with the client in `tests/client_test.c`:

| Test | What it covers |
|---|---|
| `compositor-client-single` | one headless output: windows, popups, maximize/fullscreen/minimize, virtual keyboard + pointer, snapping to edges and to another window, Alt+Return terminal, keyboard repeat, `xdg-output`, live config reload (errors with line numbers, autostart) |
| `compositor-client-multi` | two outputs with different size, scale and position: placement and maximize per output, `focus = follow-mouse`, move/focus to the next output, `reload-config` key, bad keyboard layout fallback |
| `compositor-client-nested` | the single scenario with `sfwc` using the wayland backend inside a headless `sfwc`, like starting it in another Wayland session |

Input is injected with the wlroots virtual keyboard/pointer protocols. Those let any client
inject input, so sfwc only exposes them when started with `SFWC_ENABLE_VIRTUAL_INPUT=1`
(the script sets it). The protocol XML files come from the wlroots subproject; with a system
wlroots the input part is skipped (meson prints a warning). On failure the script prints the
compositor's log, which runs at `SFWC_LOG_LEVEL=debug`.

A test that has never failed proves little: when adding a check, break the expectation once
and confirm it goes red.

## Releases

1. Move the finished milestone's entries into a new `## vX.Y.0 – date – title` section
   of `CHANGELOG.md` and set `version:` in `meson.build` (and `themes/meson.build`).
2. Merge/push, wait for CI to be green on that commit.
3. `git tag -a vX.Y.0 -m "SFWC vX.Y.0" && git push origin vX.Y.0`
   The `release` workflow checks that tag and `meson.build` agree, then publishes a
   (pre-)release with the changelog section and a source tarball + sha256.
