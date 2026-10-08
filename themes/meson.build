# Optional theme package for SFWC. Build/install separately from the compositor:
#   meson setup build-themes themes && meson install -C build-themes
project('sfwc-themes', version: '0.0.1', license: 'MIT', meson_version: '>=0.59.0')

install_data('default.theme', 'light.theme',
  install_dir: get_option('datadir') / 'sfwc' / 'themes')
