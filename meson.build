# Unit-test harness only. The driver itself is built inside a libfprint
# tree (scripts/integrate.py); this project compiles the same sources
# (driver/goodix55b4/meson.build) into a static library and links the tests
# in tests/ against it and a libfprint build for the few libfprint symbols
# they need. Use scripts/run-tests.sh rather than calling meson directly.
project('goodix55b4-tests', 'c',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'])

cc = meson.get_compiler('c')
lf_src = get_option('libfprint_src')
lf_build = get_option('libfprint_build')
if lf_src == '' or lf_build == ''
  error('set -Dlibfprint_src and -Dlibfprint_build (see scripts/run-tests.sh)')
endif

# What the driver's meson.build expects from libfprint's.
drivers = ['goodix55b4']
optional_deps = []
subdir('driver/goodix55b4')

deps = [
  dependency('glib-2.0'),
  dependency('gio-2.0'),
  dependency('gusb'),
  dependency('openssl'),
  dependency('libdeflate'),
  cc.find_library('fprint-2', dirs: lf_build / 'libfprint'),
]
# Plain -I: the libfprint tree may live inside this one (.build/), where
# meson refuses absolute include_directories().
lf_inc = ['-I' + lf_src / 'libfprint', '-I' + lf_build / 'libfprint']
add_project_arguments('-Wno-unused-parameter', '-Wno-sign-compare',
                      '-Wno-missing-field-initializers', language: 'c')

driver = static_library('goodix55b4',
  goodix55b4_sources,
  c_args: goodix55b4_cflags + lf_inc,
  dependencies: deps,
)

test_env = environment()
test_env.set('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
test_env.set('UBSAN_OPTIONS', 'print_stacktrace=1:halt_on_error=1')
# Real frames are personal data and stay outside the repository.
dataset = get_option('dataset')
if dataset != ''
  test_env.set('GOODIX55B4_DATASET', dataset)
endif

foreach t : ['goodixtls', 'goodix-proto', 'goodix55b4-calib',
             'goodix55b4-state', 'goodix55b4-study', 'goodix55b4-writer',
             'goodix55b4-dataset']
  exe = executable('test-' + t, 'tests/test-' + t + '.c',
    c_args: goodix55b4_cflags + lf_inc + ['-Werror'],
    link_with: driver,
    dependencies: deps,
    build_rpath: lf_build / 'libfprint',
  )
  # The dataset suite runs the engine on real frames: slow under ASan.
  test(t, exe, env: test_env, protocol: 'tap',
       timeout: t == 'goodix55b4-dataset' ? 1800 : 30)
endforeach
