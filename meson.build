project('obs-xdg-portal', 'c')

shared_library('obs-xdg-portal',
  'obs-xdg-portal.c',
  name_prefix : '',
  dependencies : [
    meson.get_compiler('c').find_library('obs'),
    dependency('gio-2.0'),
    dependency('gio-unix-2.0'),
    dependency('gstreamer-1.0'),
    dependency('gstreamer-app-1.0'),
    dependency('gstreamer-video-1.0'),
  ],
  install : true,
  install_dir : 'lib/obs-plugins'
)
