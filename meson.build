project('sfwc', 'c',
  version: '0.2.0',
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
    '-DINI_MAX_LINE=1024',
  ],
  include_directories: include_directories('third_party/inih'))
inih_dep = declare_dependency(
  link_with: inih,
  include_directories: include_directories('third_party/inih'))

xkbcommon = dependency('xkbcommon')

# Parsers, independent of wlroots so they can be unit-tested everywhere.
inifile_lib = static_library('sfwc_inifile', 'src/inifile.c', dependencies: inih_dep)
inifile_dep = declare_dependency(
  link_with: inifile_lib,
  include_directories: include_directories('src'),
  dependencies: inih_dep)

config_lib = static_library('sfwc_config', 'src/config.c',
  dependencies: [inifile_dep, xkbcommon])
config_dep = declare_dependency(
  link_with: config_lib,
  include_directories: include_directories('src'),
  dependencies: [inifile_dep, xkbcommon])

theme_lib = static_library('sfwc_theme', 'src/theme.c', dependencies: inifile_dep)
theme_dep = declare_dependency(
  link_with: theme_lib,
  include_directories: include_directories('src'),
  dependencies: inifile_dep)

libm = meson.get_compiler('c').find_library('m')

anim_lib = static_library('sfwc_anim', 'src/anim.c')
anim_dep = declare_dependency(
  link_with: anim_lib,
  include_directories: include_directories('src'))

cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
fontconfig = dependency('fontconfig')

deco_lib = static_library('sfwc_deco', 'src/deco.c',
  dependencies: [theme_dep, cairo, pangocairo, libm])
deco_dep = declare_dependency(
  link_with: deco_lib,
  include_directories: include_directories('src'),
  dependencies: [theme_dep, cairo, pangocairo, libm])

if get_option('compositor')
  # Uses the system wlroots 0.18 if present, otherwise builds it from
  # subprojects/wlroots.wrap.
  wlroots = dependency('wlroots-0.18', fallback: ['wlroots', 'wlroots_dep'],
    default_options: ['examples=false'])
  wayland_server = dependency('wayland-server')
  libdrm = dependency('libdrm')

  # wlroots headers include xdg-shell-protocol.h, which we generate ourselves.
  wayland_scanner = find_program('wayland-scanner', native: true)
  wayland_protos = dependency('wayland-protocols', version: '>=1.32', native: true)
  xdg_shell_xml = wayland_protos.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
  xdg_shell_h = custom_target('xdg-shell-protocol.h',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

  sfwc_exe = executable('sfwc',
    'src/main.c', xdg_shell_h,
    dependencies: [wlroots, wayland_server, xkbcommon, libm, libdrm, config_dep, theme_dep, deco_dep, anim_dep],
    c_args: ['-Wno-unused-parameter'],
    install: true)

  install_data('config/sfwc.conf', install_dir: get_option('datadir') / 'sfwc')
endif

subdir('tests')
