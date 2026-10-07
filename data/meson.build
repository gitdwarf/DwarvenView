# Desktop file
install_data('dwarvenview.desktop',
  install_dir : join_paths(get_option('datadir'), 'applications'))

# SVG icon -- scales to any size, hicolor is the correct theme
install_data('../src/dwarvenview-icon.svg',
  rename : 'dwarvenview.svg',
  install_dir : join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'))

# AppStream metainfo: version and date come from the top-level meson.build
configure_file(
  input : 'dwarvenview.metainfo.xml.in',
  output : 'dwarvenview.metainfo.xml',
  configuration : {'VERSION': meson.project_version(), 'DATE': release_date},
  install : true,
  install_dir : join_paths(get_option('datadir'), 'metainfo'))
