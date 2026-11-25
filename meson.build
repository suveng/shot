project('shot', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.61',
  default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

gtk = dependency('gtk4', version: '>= 4.12')
math = meson.get_compiler('c').find_library('m', required: false)

executable('shot', 'src/shot.c',
  dependencies: [gtk, math],
  install: true)

subdir('data')
