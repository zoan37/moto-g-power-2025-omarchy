project('mtk-stateful-va', 'c',
        version: '0.1',
        license: 'GPL-3.0-or-later',
        default_options: [
          'c_std=gnu11',
          'buildtype=release',
          'warning_level=2',
        ])

libva_dep = dependency('libva', version: '>= 1.10.0')
libdrm_dep = dependency('libdrm')
threads_dep = dependency('threads')

if get_option('driverdir') != ''
  driverdir = get_option('driverdir')
else
  driverdir = libva_dep.get_variable(pkgconfig: 'driverdir',
                                     default_value: get_option('prefix') / get_option('libdir') / 'dri')
endif

shared_module('mtk_stateful_drv_video',
  'src/driver.c',
  'src/handles.c',
  'src/buffer.c',
  'src/image.c',
  'src/surface.c',
  'src/context.c',
  'src/detile.c',
  'src/h264.c',
  'src/vp9.c',
  name_prefix: '',
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: driverdir)
