describe_h = configure_file(
  output: 'describe.h',
  configuration: configuration_data({
    'EPICS_HOST_ARCH': f'"@epics_host_arch@"',
    'T_A': f'"@epics_target_arch@"',
    'OS_CLASS': f'"@epics_os_class@"',
    'CMPLR_CLASS': f'"@epics_cmplr_class@"',
  }),
)

vcs_h = configure_file(
  output: 'pvxsVCS.h',
  configuration: configuration_data({
  }),
)

version = meson.project_version().split('.')
version_num_h = configure_file(
  output: 'versionNum.h',
  configuration: configuration_data({
    'PVXS_MAJOR_VERSION': version[0],
    'PVXS_MINOR_VERSION': version[1],
    'PVXS_MAINTENANCE_VERSION': version[2],
  }),
)

# HACK: this installs generated headers in a 'pvxs' subdir
# See: https://github.com/mesonbuild/meson/issues/2320
fs = import('fs')
run_command(
  [
    'sh',
    '-c',
    'mkdir @0@/pvxs; cp @1@/@2@ @0@/pvxs/'.format(
      meson.current_build_dir(),
      fs.parent(meson.current_build_dir()),
      version_num_h,
    ),
  ],
  check: true,
)

libevent = dependency('libevent_pthreads')

cpp = meson.get_compiler('cpp')

extra_cpp_args = []

if cpp.has_argument('-fvisibility=hidden')
  extra_cpp_args += ['-fvisibility=hidden']
endif

if cpp.has_argument('-fvisibility-inlines-hidden')
  extra_cpp_args += ['-fvisibility-inlines-hidden']
endif

if host_machine.system() == 'windows'
  ost_sock_ext = 'os/WIN32/osdSockExt.cpp'
else
  ost_sock_ext = 'os/default/osdSockExt.cpp'
endif

libpvxs = library(
  'pvxs',
  [
    'describe.cpp',
    'log.cpp',
    'unittest.cpp',
    'util.cpp',
    'osgroups.cpp',
    'sharedarray.cpp',
    'bitmask.cpp',
    'type.cpp',
    'data.cpp',
    'datafmt.cpp',
    'pvrequest.cpp',
    'dataencode.cpp',
    'nt.cpp',
    'evhelper.cpp',
    'udp_collector.cpp',

    ost_sock_ext,

    'config.cpp',
    'conn.cpp',

    'server.cpp',
    'serverconn.cpp',
    'serverchan.cpp',
    'serverintrospect.cpp',
    'serverget.cpp',
    'servermon.cpp',
    'serversource.cpp',
    'sharedpv.cpp',

    'client.cpp',
    'clientreq.cpp',
    'clientconn.cpp',
    'clientintrospect.cpp',
    'clientget.cpp',
    'clientmon.cpp',
    'clientdiscover.cpp',
  ],
  cpp_args: ['-DPVXS_API_BUILDING', '-DPVXS_ENABLE_EXPERT_API'] + extra_cpp_args,
  dependencies: [epics_base, libCom, libevent],
  version: meson.project_version(),
  install: true,
  install_dir: lib_install_dir,
  # HACK: Workaround https://github.com/mesonbuild/meson/issues/6541
  install_rpath: epics_base_libdir,
)

install_headers(
  [
    version_num_h,
    'pvxs/version.h',
    'pvxs/log.h',
    'pvxs/unittest.h',
    'pvxs/util.h',
    'pvxs/sharedArray.h',
    'pvxs/data.h',
    'pvxs/nt.h',
    'pvxs/netcommon.h',
    'pvxs/server.h',
    'pvxs/srvcommon.h',
    'pvxs/sharedpv.h',
    'pvxs/source.h',
    'pvxs/client.h',
  ],
  subdir: 'pvxs',
)

pvxs_dep = declare_dependency(
  include_directories: '.',
  dependencies: [libevent],
  link_with: [libpvxs],
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(libpvxs)
