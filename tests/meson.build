test_ini = executable('test_ini', 'test_ini.c',
  dependencies: inih_dep,
  c_args: ['-DSRC_ROOT="' + meson.project_source_root() + '"'])
test('config-and-theme-parsing', test_ini)

test_config = executable('test_config', 'test_config.c',
  dependencies: config_dep,
  c_args: ['-DSRC_ROOT="' + meson.project_source_root() + '"'])
test('config-parser', test_config)

test_theme = executable('test_theme', 'test_theme.c',
  dependencies: theme_dep,
  c_args: ['-DSRC_ROOT="' + meson.project_source_root() + '"'])
test('theme-parser', test_theme)

test_deco = executable('test_deco', 'test_deco.c',
  dependencies: [deco_dep, fontconfig])
test('decoration-rendering', test_deco)

test_template = executable('test_template', 'test_template.c', dependencies: [template_dep, theme_dep])
test('template-rendering', test_template)

test('theme-apply-helper', find_program('sh'), args: [files('test_apply.sh'), theme_apply_exe])

test('shipped-templates', find_program('sh'),
  args: [files('test_shipped_templates.sh'), theme_apply_exe, meson.project_source_root()])

test_anim = executable('test_anim', 'test_anim.c', dependencies: [anim_dep, libm])
test('animation-math', test_anim)

if get_option('compositor')
  # End-to-end: run sfwc headless and talk to it with a real Wayland client.
  wayland_client = dependency('wayland-client')
  xdg_shell_client_h = custom_target('xdg-shell-client-protocol.h',
    input: xdg_shell_xml,
    output: 'xdg-shell-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  xdg_shell_c = custom_target('xdg-shell-protocol.c',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  xdg_output_xml = wayland_protos.get_variable('pkgdatadir') / 'unstable/xdg-output/xdg-output-unstable-v1.xml'
  client_sources = [xdg_shell_client_h, xdg_shell_c]
  xdg_deco_xml = wayland_protos.get_variable('pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
  client_sources += custom_target('xdg-decoration-unstable-v1-client-protocol.h',
    input: xdg_deco_xml, output: 'xdg-decoration-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  client_sources += custom_target('xdg-decoration-unstable-v1-protocol.c',
    input: xdg_deco_xml, output: 'xdg-decoration-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  client_sources += custom_target('xdg-output-unstable-v1-client-protocol.h',
    input: xdg_output_xml, output: 'xdg-output-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  client_sources += custom_target('xdg-output-unstable-v1-protocol.c',
    input: xdg_output_xml, output: 'xdg-output-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  session_lock_xml = wayland_protos.get_variable('pkgdatadir') / 'staging/ext-session-lock/ext-session-lock-v1.xml'
  client_sources += custom_target('ext-session-lock-v1-client-protocol.h',
    input: session_lock_xml, output: 'ext-session-lock-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  client_sources += custom_target('ext-session-lock-v1-protocol.c',
    input: session_lock_xml, output: 'ext-session-lock-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  client_deps = [wayland_client]
  client_args = ['-Wno-unused-parameter']

  # Virtual keyboard/pointer protocol files ship with the wlroots sources. They are
  # only available when wlroots is built from the subproject; otherwise the input
  # part of the test is skipped.
  fs = import('fs')
  wlr_protocol_dir = meson.project_source_root() / 'subprojects/wlroots/protocol'
  vkbd_xml = wlr_protocol_dir / 'virtual-keyboard-unstable-v1.xml'
  vptr_xml = wlr_protocol_dir / 'wlr-virtual-pointer-unstable-v1.xml'
  if fs.exists(vkbd_xml) and fs.exists(vptr_xml)
    # Paths are passed as plain strings: meson's sandbox rejects `input:` files that
    # live inside a nested subproject.
    foreach name : ['virtual-keyboard-unstable-v1', 'wlr-virtual-pointer-unstable-v1', 'wlr-screencopy-unstable-v1',
                  'wlr-layer-shell-unstable-v1']
      xml = wlr_protocol_dir / name + '.xml'
      client_sources += custom_target(name + '-client-protocol.h',
        output: name + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', xml, '@OUTPUT@'])
      client_sources += custom_target(name + '-protocol.c',
        output: name + '-protocol.c',
        command: [wayland_scanner, 'private-code', xml, '@OUTPUT@'])
    endforeach
    client_deps += xkbcommon
    client_args += '-DHAVE_VIRTUAL_INPUT'
  else
    warning('wlroots protocol files not found; input tests are skipped')
  endif

  client_test = executable('client_test', 'client_test.c', client_sources,
    dependencies: client_deps, c_args: client_args)
  foreach mode : ['single', 'multi', 'nested', 'deco', 'anim', 'layers', 'workspaces', 'lock']
    test('compositor-client-' + mode, find_program('bash'),
      args: [files('run_client_test.sh'), sfwc_exe, client_test, mode],
      timeout: 120)
  endforeach
endif
