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
