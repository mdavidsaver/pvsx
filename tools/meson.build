foreach e : [
    ['pvxvct', ['./pvxvct.cpp']],
    ['pvxinfo', ['./info.cpp']],
    ['pvxget', ['./get.cpp']],
    ['pvxmonitor', ['./monitor.cpp']],
    ['pvxput', ['./put.cpp']],
    ['pvxcall', ['./call.cpp']],
    ['pvxlist', ['./list.cpp']],
    ['pvxmshim', ['./mshim.cpp']],
  ]
  executable(
    e[0],
    e[1],
    dependencies: [epics_base, libCom, pvxs_dep],
    install: true,
    install_dir: bin_install_dir,
    # HACK: Workaround https://github.com/mesonbuild/meson/issues/6541
    install_rpath: epics_base_libdir,
  )
endforeach
