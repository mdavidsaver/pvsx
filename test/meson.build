epics_version = epics_base.version()

tests = [
  ['testsock', ['./testsock.cpp']],
  ['testutil', ['./testutil.cpp']],
  ['testev', ['./testev.cpp']],
  ['testlog', ['./testlog.cpp']],
  ['testudp', ['./testudp.cpp']],
  ['testshared', ['./testshared.cpp']],
  ['testbitmask', ['./testbitmask.cpp']],
  ['testxcode', ['./testxcode.cpp']],
  ['testtype', ['./testtype.cpp']],
  ['testdata', ['./testdata.cpp']],
  ['testnt', ['./testnt.cpp']],
  ['testconfig', ['./testconfig.cpp']],
  ['testwild', ['./testwild.cpp']],
  ['testpvreq', ['./testpvreq.cpp']],
  ['testinfo', ['./testinfo.cpp']],
  ['testget', ['./testget.cpp']],
  ['testmon', ['./testmon.cpp']],
  ['testmonpipe', ['./testmonpipe.cpp']],
  ['testput', ['./testput.cpp']],
  ['testrpc', ['./testrpc.cpp']],
  # very slow and dependent on host network config.
  #['testdiscover', ['./testdiscover.cpp']],
  ['testnamesrv', ['./testnamesrv.cpp']],
  ['test1000', ['./test1000.cpp']],
  ['testendian', ['./testendian.cpp']],
  ['testudpfwd', ['./testudpfwd.cpp']],
]

# TODO: add tests that depend on registerRecordDeviceDriver

foreach t : tests
  test = executable(
    t[0],
    t[1],
    dependencies: [epics_base, libCom, pvxs_dep],
  )
  test(t[0], test, is_parallel: true)
endforeach

if epics_version.version_compare('>=7.0.0.0')
  benchmark = executable(
    'benchdata',
    ['./benchdata.cpp'],
    dependencies: [epics_base, libCom, pvxs_dep],
  )
  benchmark('benchdata', benchmark)
endif

# Development utilities, not tests
foreach e : [
    ['mcat', ['./mcat.cpp']],
    ['spam', ['./spam.cpp']],
    ['eatspam', ['./eatspam.cpp']],
  ]
  executable(
    e[0],
    e[1],
    dependencies: [epics_base, libCom, pvxs_dep],
  )
endforeach
