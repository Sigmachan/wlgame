project('wlgame', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'warning_level=2',
    'c_std=c17',
  ],
)

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-Wno-unused-parameter',
  language: 'c',
)

cc = meson.get_compiler('c')

wlroots    = dependency('wlroots-0.21')
wayland    = dependency('wayland-server')
xkbcommon  = dependency('xkbcommon')
math_dep   = cc.find_library('m', required: false)

sources = files(
  'src/main.c',
  'src/server.c',
  'src/output.c',
  'src/view.c',
  'src/input.c',
  'src/nvidia.c',
)

executable('wlgame', sources,
  dependencies: [wlroots, wayland, xkbcommon, math_dep],
  include_directories: include_directories('include'),
  install: true,
)

install_data('wlgame.desktop', install_dir: 'share/wayland-sessions',
  install_tag: 'runtime')
