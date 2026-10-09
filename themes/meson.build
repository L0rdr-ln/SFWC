# Optional basic theme package for SFWC. Build/install separately from the compositor:
#   meson setup build-themes themes && meson install -C build-themes
project('sfwc-themes', version: '0.6.0', license: 'MIT', meson_version: '>=0.59.0')

# Only the two basic themes live here; more (nord, gruvbox, dracula, ...) are the theme pack of
# the sfwc-plugins repository.
install_data(
  'default.theme', 'light.theme',
  install_dir: get_option('datadir') / 'sfwc' / 'themes')

install_subdir('templates', install_dir: get_option('datadir') / 'sfwc')
