project('dwarvenview', 'c',
  version : '0.5.58',
  default_options : ['warning_level=2', 'c_std=c11'])

# Bumped with the version above; used only by the AppStream release entry.
release_date = '2026-10-15'

conf = configuration_data()
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(output : 'config.h', configuration : conf)

# src/ exports dwarvenview_sources and dwarvenview_deps into this scope.
# The target is declared here, not in src/, because meson mirrors the source
# subdirectory into the build tree: declared in src/ it lands in build/src/.
subdir('src')

executable('dwarvenview',
  dwarvenview_sources,
  include_directories : include_directories('.'),
  dependencies : dwarvenview_deps,
  install : true)

subdir('data')

# The tests ship as a separate archive. Unpack it into this directory first.
if get_option('tests')
  if not import('fs').is_dir('tests')
    error('-Dtests=true needs the test archive: unpack dwarvenview-tests-' + meson.project_version() + '.zip into the source directory.')
  endif
  subdir('tests')
endif
