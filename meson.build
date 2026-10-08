project('sfwc', 'c',
  version: '0.0.1',
  license: 'MIT',
  meson_version: '>=0.63.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# Config/theme parser (vendored inih). Comments are full-line only.
inih = static_library('inih', 'third_party/inih/ini.c',
  c_args: [
    '-DINI_ALLOW_INLINE_COMMENTS=0',
    '-DINI_ALLOW_MULTILINE=0',
    '-DINI_STOP_ON_FIRST_ERROR=0',
    '-DINI_USE_STACK=1',
  ],
  include_directories: include_directories('third_party/inih'))
inih_dep = declare_dependency(
  link_with: inih,
  include_directories: include_directories('third_party/inih'))

if get_option('compositor')
  # Uses the system wlroots 0.18 if present, otherwise builds it from
  # subprojects/wlroots.wrap.
  wlroots = dependency('wlroots-0.18', fallback: ['wlroots', 'wlroots_dep'],
    default_options: ['examples=false'])
  wayland_server = dependency('wayland-server')
  xkbcommon = dependency('xkbcommon')

  executable('sfwc',
    'src/main.c',
    dependencies: [wlroots, wayland_server, xkbcommon, inih_dep],
    install: true)

  install_data('config/sfwc.conf', install_dir: get_option('datadir') / 'sfwc')
endif

subdir('tests')
