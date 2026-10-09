# Test plugins for the plugin API (see docs/PLUGINS.md); real plugins live in the sfwc-plugins
# repository. Shared objects loaded at run time, so they use the compositor's wlroots.
plugin_args = ['-Wno-unused-parameter']
plugin_deps = [wlroots, wayland_server, libm]
plugin_inc = include_directories('../include')

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
