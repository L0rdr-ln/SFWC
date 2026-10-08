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

# The compositor build needs a newer xkbcommon (wlroots 0.20 asks for >= 1.8); the parsers only
# need the keysym names, any version will do.
xkbcommon = dependency('xkbcommon',
  version: get_option('compositor') ? '>=1.8.0' : '>=1.4.0',
  fallback: ['xkbcommon', 'xkbcommon_dep'],
  default_options: ['enable-docs=false', 'enable-tools=false', 'enable-x11=false',
                    'enable-wayland=false', 'enable-xkbregistry=false',
                    'enable-bash-completion=false'])

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

template_lib = static_library('sfwc_template', 'src/template.c', dependencies: [theme_dep, libm])
template_dep = declare_dependency(
  link_with: template_lib,
  include_directories: include_directories('src'),
  dependencies: [theme_dep, libm])

# Renders the theme's templates for waybar, fuzzel, ... (does not need wlroots).
theme_apply_exe = executable('sfwc-theme-apply', 'src/sfwc-theme-apply.c',
  dependencies: [template_dep, theme_dep], install: true)
install_man('man/sfwc-theme-apply.1')

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
  # Uses the system wlroots 0.20 if present, otherwise builds it from
  # subprojects/wlroots.wrap.
  wlroots = dependency('wlroots-0.20', fallback: ['wlroots', 'wlroots_dep'],
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
  # wlr_layer_shell_v1.h includes the server header of the wlr-layer-shell protocol.
  layer_shell_h = custom_target('wlr-layer-shell-unstable-v1-protocol.h',
    output: 'wlr-layer-shell-unstable-v1-protocol.h',
    command: [wayland_scanner, 'server-header',
              meson.project_source_root() / 'subprojects/wlroots/protocol/wlr-layer-shell-unstable-v1.xml',
              '@OUTPUT@'])

  # wlr_session_lock_v1.h includes the server header of ext-session-lock.
  session_lock_h = custom_target('ext-session-lock-v1-protocol.h',
    input: wayland_protos.get_variable('pkgdatadir') / 'staging/ext-session-lock/ext-session-lock-v1.xml',
    output: 'ext-session-lock-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

  sfwc_sources = files(
    'src/main.c', 'src/settings.c', 'src/animate.c', 'src/window.c', 'src/workspace.c',
    'src/decoration.c', 'src/output.c', 'src/input.c', 'src/actions.c', 'src/cursor.c',
    'src/layers.c', 'src/lock.c', 'src/foreign.c', 'src/idle.c')

  sfwc_exe = executable('sfwc',
    sfwc_sources, xdg_shell_h, layer_shell_h, session_lock_h,
    dependencies: [wlroots, wayland_server, xkbcommon, libm, libdrm, config_dep, theme_dep, template_dep, deco_dep, anim_dep],
    c_args: ['-Wno-unused-parameter'],
    install: true)

  install_data('config/sfwc.conf', install_dir: get_option('datadir') / 'sfwc')
  install_data('data/sfwc.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
  install_man('man/sfwc.1', 'man/sfwc.conf.5')
  install_data('README.md', 'CHANGELOG.md', 'docs/CONFIGURATION.md', 'docs/THEMES.md',
    'docs/ARCHITECTURE.md', install_dir: get_option('datadir') / 'doc' / 'sfwc')
endif

subdir('tests')
