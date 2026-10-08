test_ini = executable('test_ini', 'test_ini.c',
  dependencies: inih_dep,
  c_args: ['-DSRC_ROOT="' + meson.project_source_root() + '"'])
test('config-and-theme-parsing', test_ini)

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
  client_test = executable('client_test', 'client_test.c', xdg_shell_client_h, xdg_shell_c,
    dependencies: wayland_client, c_args: ['-Wno-unused-parameter'])
  test('compositor-client', find_program('bash'),
    args: [files('run_client_test.sh'), sfwc_exe, client_test],
    timeout: 60)
endif
