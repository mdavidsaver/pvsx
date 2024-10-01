epics_version = epics_base.version()

dbd_file = './pvxs@0@x.dbd'.format(epics_version.split('.')[0])

install_data([dbd_file], rename: ['pvxsIoc.dbd'], install_dir: dbd_install_dir)

srcs = ['./iochooks.cpp']

if epics_version.version_compare('>=3.15.0.0')
  srcs += [
    'credentials.cpp',
    'channel.cpp',
    'demo.cpp',
    'dberrormessage.cpp',
    'imagedemo.c',
    'iocsource.cpp',
    'localfieldlog.cpp',
    'securityclient.cpp',
    'singlesource.cpp',
    'singlesourcehooks.cpp',
    'singlesrcsubscriptionctx.cpp',
    'typeutils.cpp',
  ]
else
  srcs += [
    'dummysingle.cpp'
  ]
endif

if epics_version.version_compare('>=7.0.0.0')
  srcs += [
    'field.cpp',
    'fielddefinition.cpp',
    'fieldname.cpp',
    'fieldsubscriptionctx.cpp',
    'group.cpp',
    'groupconfigprocessor.cpp',
    'groupprocessorcontext.cpp',
    'groupsource.cpp',
    'groupsourcehooks.cpp',
    'pvalink.cpp',
    'pvalink_channel.cpp',
    'pvalink_jlif.cpp',
    'pvalink_link.cpp',
    'pvalink_lset.cpp',
  ]
else
  srcs += [
    'dummygroup.cpp'
  ]
endif

libpvxsIoc = library(
  'pvxsIoc',
  srcs,
  cpp_args: ['-DPVXS_IOC_API_BUILDING'],
  dependencies: [epics_base, libCom, libdbCore, pvxs_dep],
  version: meson.project_version(),
  install: true,
  install_dir: lib_install_dir,
  # HACK: Workaround https://github.com/mesonbuild/meson/issues/6541
  install_rpath: epics_base_libdir,
)

install_headers(
  ['./pvxs/iochooks.h'],
  subdir: 'pvxs',
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(libpvxsIoc)
