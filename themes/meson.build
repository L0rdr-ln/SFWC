# Optional theme package for SFWC. Build/install separately from the compositor:
#   meson setup build-themes themes && meson install -C build-themes
project('sfwc-themes', version: '0.2.0', license: 'MIT', meson_version: '>=0.59.0')

install_data(
  'default.theme', 'light.theme',
  'nord.theme', 'gruvbox-dark.theme', 'dracula.theme', 'tokyo-night.theme', 'rose-pine.theme',
  'solarized-dark.theme', 'solarized-light.theme', 'high-contrast.theme',
  install_dir: get_option('datadir') / 'sfwc' / 'themes')

install_subdir('templates', install_dir: get_option('datadir') / 'sfwc')
