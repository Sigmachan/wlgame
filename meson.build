project('wlgame', 'c',
  version: '0.2.0',
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

wlroots   = dependency('wlroots-0.21')
wayland   = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
libdrm    = dependency('libdrm')
vulkan    = dependency('vulkan')
math_dep  = cc.find_library('m', required: false)

# ── XWayland (optional but strongly recommended for game compat) ─────────
xcb        = dependency('xcb',        required: false)
xwayland   = dependency('xwayland',   required: false)
have_xwayland = xcb.found() and xwayland.found()
if have_xwayland
  add_project_arguments('-DHAVE_XWAYLAND', language: 'c')
  message('XWayland support: enabled')
else
  warning('XWayland disabled — xcb or xwayland not found')
endif

# ── GLSL → SPIR-V ───────────────────────────────────────────────────────
glslang = find_program('glslangValidator', required: false)
shader_dir_installed = get_option('prefix') / get_option('datadir') / 'wlgame' / 'shaders'
add_project_arguments(
  '-DWLGAME_SHADER_DIR="' + shader_dir_installed + '"',
  language: 'c',
)

spv_targets = []
if glslang.found()
  foreach name : ['cas', 'nis', 'fsr1_easu', 'fsr1_rcas']
    spv_targets += custom_target(name + '_spv',
      input:       'shaders/' + name + '.comp',
      output:      name + '.comp.spv',
      command:     [glslang, '-V', '--target-env', 'vulkan1.2', '@INPUT@', '-o', '@OUTPUT@'],
      build_by_default: true,
      install:     true,
      install_dir: get_option('datadir') / 'wlgame' / 'shaders',
    )
  endforeach
  message('Shaders will be compiled to SPIR-V (glslangValidator found)')
else
  warning('glslangValidator not found — FSR/NIS/CAS shaders will not be compiled')
endif

# ── sources ──────────────────────────────────────────────────────────────
sources = files(
  'src/main.c',
  'src/server.c',
  'src/output.c',
  'src/view.c',
  'src/input.c',
  'src/gpu.c',
  'src/upscale.c',
  'src/layer.c',
)

deps = [wlroots, wayland, xkbcommon, libdrm, vulkan, math_dep]

if have_xwayland
  sources += files('src/xwayland.c')
  deps += [xwayland, xcb]
endif

executable('wlgame', sources,
  dependencies: deps,
  include_directories: include_directories('include'),
  install: true,
)

install_data('wlgame.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
  install_tag: 'runtime',
)
