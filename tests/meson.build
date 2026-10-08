test_ini = executable('test_ini', 'test_ini.c',
  dependencies: inih_dep,
  c_args: ['-DSRC_ROOT="' + meson.project_source_root() + '"'])
test('config-and-theme-parsing', test_ini)
