project('sfwc', 'c',
  version: '0.0.1',
  license: 'MIT',
  meson_version: '>=0.59.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

executable('sfwc',
  'src/main.c',
  dependencies: [wlroots, wayland_server, xkbcommon],
  install: true)

install_data('config/sfwc.conf', install_dir: get_option('datadir') / 'sfwc')
