project(
  'pvxs',
  ['cpp', 'c'],
  version: '1.3.1',
  license: 'EPICS',
  # license_files: './LICENSE',
  default_options: [
    'default_library=both',
  ],
)

# Build Machine as in Autotools terminology: the machine that will build
# the final code, which in EPICS terminology is the Host Machine.
gnu_host_arch = build_machine.cpu() + '-' + build_machine.system()
epics_host_arch = run_command(
  'EpicsHostArch.pl',
  '-g',
  gnu_host_arch,
  capture: true,
  check: true
).stdout().strip()

# Host Machine as in Autotools terminology: the machine that will host and run
# the final code, which in EPICS terminology is the Target Machine.
gnu_target_arch = host_machine.cpu() + '-' + host_machine.system()
epics_target_arch = run_command(
  'EpicsHostArch.pl',
  '-g',
  gnu_target_arch,
  capture: true,
  check: true
).stdout().strip()

if get_option('epics_install_hierarchy')
  bin_install_dir = get_option('bindir') / epics_target_arch
  lib_install_dir = get_option('libdir') / epics_target_arch
else
  bin_install_dir = get_option('bindir')
  lib_install_dir = get_option('libdir')
endif

dbd_install_dir = get_option('prefix') / 'dbd'
db_install_dir = get_option('prefix') / 'db'

epics_base = dependency('epics-base-' + epics_target_arch)
epics_base_libdir = epics_base.get_pkgconfig_variable('libdir')
epics_os_class = epics_base.get_pkgconfig_variable('OS_CLASS')
epics_cmplr_class = epics_base.get_pkgconfig_variable('CMPLR_CLASS')

compiler = meson.get_compiler('cpp')

libca = compiler.find_library('ca', dirs: [epics_base_libdir])
libCom = compiler.find_library('Com', dirs: [epics_base_libdir])
libdbCore = compiler.find_library('dbCore', dirs: [epics_base_libdir])

subdir('src')
subdir('tools')
subdir('ioc')
subdir('test')
subdir('example')
