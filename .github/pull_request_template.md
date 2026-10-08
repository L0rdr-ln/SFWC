## What and why

<!-- One feature or fix per pull request. -->

## How it was tested

<!-- `meson test -C build --print-errorlogs` output, and for behavior changes the test that covers it.
     A new check should be seen failing once before it is trusted. -->

- [ ] `meson test -C build` passes (sanitizer build if you touched C code)
- [ ] docs and `CHANGELOG.md` updated if behavior or configuration changed
- [ ] new themes: `tools/theme-preview.py` run, listed in `README.md` and `themes/meson.build`
