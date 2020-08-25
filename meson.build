project('obs-xdg-portal', 'c')

shared_library('obs-xdg-portal',
  'obs-xdg-portal.c',
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
