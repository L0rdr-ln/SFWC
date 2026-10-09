# Plugins for sfwc (see docs/PLUGINS.md). They are shared objects loaded at run time, so they use
# the compositor's wlroots; built only together with the compositor.
plugin_args = ['-Wno-unused-parameter']
plugin_deps = [wlroots, wayland_server, libm]
plugin_inc = include_directories('../include')

# NAME.so is what the config's `load = NAME` looks for.
shared_module('wobbly', 'wobbly/wobbly.c', 'wobbly/mesh.c', xdg_shell_h, layer_shell_h, session_lock_h,
  name_prefix: '',
  dependencies: plugin_deps,
  include_directories: plugin_inc,
  c_args: plugin_args,
  install: true, install_dir: plugin_dir)

# A plugin that only logs what the compositor tells it (used by the tests, not installed).
shared_module('hooktest', 'hooktest/hooktest.c', xdg_shell_h, layer_shell_h, session_lock_h,
  name_prefix: '',
  dependencies: plugin_deps,
  include_directories: plugin_inc,
  c_args: plugin_args,
  install: false)

shared_module('badplugin', 'badplugin/badplugin.c', xdg_shell_h, layer_shell_h, session_lock_h,
  name_prefix: '',
  dependencies: plugin_deps,
  include_directories: plugin_inc,
  c_args: plugin_args,
  install: false)
