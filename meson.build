project('obs-xdg-portal', 'c')

sources = files(
  'desktop-capture.c',
  'obs-xdg-portal.c',
  'pipewire.c',
  'utils.c',
  'window-capture.c',
)

datadir = join_paths(get_option('datadir'), 'obs', 'obs-plugins', 'obs-xdg-portal')
install_subdir('locale', install_dir: datadir)

shared_library('obs-xdg-portal',
  sources,
  name_prefix : '',
  dependencies : [
    dependency('libobs'),
    dependency('gio-2.0'),
    dependency('gio-unix-2.0'),
    dependency('libpipewire-0.3'),
    dependency('libspa-0.2'),
    dependency('xdg-desktop-portal'),
  ],
  install : true,
  install_dir : 'lib/obs-plugins'
)
