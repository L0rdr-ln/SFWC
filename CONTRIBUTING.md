# Contributing

1. Install the dependencies listed in the [README](README.md#build).
2. `meson setup build && meson compile -C build`
3. Run nested inside an existing Wayland session: `./build/sfwc`
4. Format with `clang-format -i src/*.c src/*.h` (config in `.clang-format`).
5. Keep changes small and focused; one feature per pull request.

Code style: C11, 4-space indent, Linux-style braces, no global state where a
struct passed around will do. Prefer wlroots' scene graph API for rendering.
