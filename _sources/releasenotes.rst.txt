.. _releasenotes:

Release Notes
=============

1.3.3 (UNRELEASED)
------------------

* Client: search retry step reset on channel reconnection (Anze Zagar)
* Various documentation improvements!  (Érico Nogueira)
* Fix dbLoadGroups (Érico Nogueira)
* Fix build with epics-base 7.0.7 (Rémi NICOLE)

1.3.2 (Oct 2024)
------------------

* Python 3.12 support
* client: Avoid FD leak on failed connect()
* Correctly set TCP_NODELAY on winsock
* Fix copy of Any/Union
* ioc: Workaround asTrapWrite clobbering dbChannel
* ioc: ACF fix write permit when groups are present
* server: correctly adjudicate collision bind() of specific port

1.3.1 (Dec 2023)
----------------

* Install pvxsIoc.dbd with the correct name in python build.
* No change to Makefile build.

1.3.0 (Dec 2023)
----------------

* Set TCP_NODELAY on client and server sockets.
* Disambiguate Size from Selector when decoding messages.
* QSRV
   * :ref:`qsrv2` now considered "feature complete".
   * Avoid sending 0 precision w/ integer value.
   * Add :ref:`pvalink`.

1.2.4 (Oct 2023)
----------------

* data: Relax when assigning struct[] or union[] of similar types.
* client: avoid assert() with invalid forced server name.
* client: propagate exception during early op. setup.
* client: GET yield complete updates after first.
* server: Ensure that variables bound into operation callback ``std::function<>`` are
  released promptly when that operation is ended.
* server: relax post() after finish().  Return false instead of throwing ``std::logic_error``.
* ioc: ensure db_cancel_event() before ~MonitorControlOp
   * Workaround for [db_cancel_event()](https://github.com/epics-base/epics-base/issues/423) bug.
* ioc: Fix typo preventing processing of DBR_STRING fields.
* ioc: fix group put always `dbProcess()`.
* ioc: fix block=true to DBF_ENUM.
* ioc: actually require +putorder to PUT.
* ioc: group put w/o effect is an error.
* ioc: accept record._options.process as bool
* ioc: group avoid post() empty events

1.2.3 (Sept 2023)
-----------------

* Add ``$PVXS_ENABLE_IPV6`` environment variable.  Set ``PVXS_ENABLE_IPV6=NO`` to disable usage of IPv6.
* IOC: A warning is printed if a group definition includes no ``+trigger`` mappings.
       This is likely a mis-configuration by omission which will produce more monitor updates than expected.
* `pvxs::server::ConnectOp::error` no longer blocks.
* Add `pvxs::target_information`.
* IOC: fill in ``display.precision``.
* Add :ref:`understandinggroups` section.
* Add `pvxs::nt::NTTable` helper.
* Clarify some exception messages.
* Add `pvxs::ioc::testPrepare`.

1.2.2 (June 2023)
-----------------

* IOC: improve long string detection.
* Changes to Tree output format for ``Value``.
* Applied various bug fixes for issued detected by static analysis.  All believed to be minor.

1.2.1 (June 2023)
-----------------

* Quiet erroneous MONITOR exceeds window size warning message on ``finish()``.
* Fix incomplete py build
* Add Python 3.11 builds (Alex Wells)

1.2.0 (May 2023)
----------------

* server: fix default monitor queueSize to 4
* improve localhost multicast forwarding
* server: bind both wildcard for UDP
* update bundled libevent
* Add `pvxs::nt::NTScalar::form`.
* NTEnum add ``display.description``
* client: schedule initial search use separate event from the generic work queue.  (Thomas Ives)
* client: re-define the meaning of poke()
* server: fix ``stats(reset=true)``
* prevent simple ``Any`` self-assignment
* ``Value::copyIn()`` (also ``from()`` and ``operator =``) add ``Array`` -> ``Array`` w/ implied alloc+convert
* client: fix batch pop() of exception
* fix monitor pipeline and ``finish()``
* server: add MonitorStat::nSquash
* Add *feature preview* of :ref:`qsrv2` IOC server functionality.

1.1.4 (April 2023)
------------------

* client: fix delta sync of Compound.  Regression in 1.1.0.
* Add wrappers for some libevent socket manipulations as :ref:`expertapi`.
* Improve diff output of `testStrEq`.
* msvc build compat.  ``/wd*`` compiler flags sometimes interpreted as paths.

1.1.3 (Mar 2023)
----------------

* Fix `pvxs::Value::nmembers` which erroneously returned ``0`` for ``StructA`` and ``UnionA``.
* Fix `pvxs::Value::unmark` where ``parent=true`` caused corruption.
* Change to "magic" automatic ``Union`` member selection during assignment.
* Fix `pvxs::shared_array::convertTo` converting ``Float64`` to ``Float32`` caused corruption.
* Fix TypeDef(const Value& val) for Union/UnionA/StructA
* Fix unsigned handling
* server: rework cleanup of connection, channel, and operation

1.1.2 (Feb 2023)
----------------

* SigInt fix disarm
* Value fix delta output format to actually show delta.
* add `pvxs::shared_array::thaw()`
* `pvxs::Value::lookup()` throw NoField
* Require ``setuptools_dso>=2.7a1``.

1.1.1 (Dec 2022)
----------------

* Fix protocol decoding error with TypeStore.

1.1.0 (Nov 2022)
----------------

* Client Subscription yields "complete" Value to user code.
  Unchanged fields will appear with the values most recently received,
  kept in an internal cache.  As a consequence, array and union fields
  will always be shared with this case, and can never be safety modified.
* Change Server monitor watermark meaning from edge to level trigger.
* `pvxs::SigInt` executes callback on worker thread instead of signal context.
* Added API

  * `pvxs::client::Subscription::stats` and `pvxs::server::MonitorControlOp::stats`
  * `pvxs::client::Context::close()`
  * `pvxs::TypeDef::as()` overload to change Struct -> StructA.
  * `pvxs::Value::clear()`

1.0.1 (Oct 2022)
----------------

* Fix c++17 compilation issue
* Allow TypeDef to append StructA and UnionA
* Reduce "non-existent IOID" noise
* Fix CMD_MESSAGE handling
* Fix locking of client monitor during pop()
* Calculate buffer sizes in terms of OS socket buffer limit
* Fix low water mark calculation
* Fix Timer ownership (expert API)
* Avoid ``assert()`` when client asked to search for PV names longer than 1400 bytes.

1.0.0 (Sept 2022)
-----------------

* Add `pvxs::client::ConnectBuilder::server`
* Add hold-off timer when reconnecting to a specific server.
* Fix missing closing quote when printing a ``String`` in tree format.

0.3.1 (June 2022)
-----------------

* Fix ifaddrs::ifa_addr can be NULL
* Limit beacon tracking by size as well as time

0.3.0 (May 2022)
----------------

* Fix protocol **incompatibility** with Big Endian servers.
* Add support for IPv4 multicast and IPv6 uni/multicast for UDP.  And IPv6 unicast for TCP.
  See :ref:`addrspec` for entries which may now appear in **EPICS_PVA*_ADDR_LIST**.
* PVXS now attempts to fanout unicast searches through the loopback interface, and
  to handle ``CMD_ORIGIN_TAG`` messages (aka. the local multicast hack).
* Add `pvxs::client::Context::discover` to enumerate and track PVA Servers.
* ``pvxlist`` add "continuous" mode.  (eg. ``pvxlist -v -w 0``)
  To immediately Discover new servers, then continue listening for Beacons to detect
  as server go up and down (like ``casw``).
  Also, to be gentler on your network, add ``-P`` to skip initial Discovery ping,
  and only listen for Beacons.

0.2.2 (Jan 2022)
----------------

* No functional change to libraries.
* Updates to python packaging.

0.2.1 (Oct 2021)
----------------

* Bug fixes

 * Fix `pvxsmonitor` hang when interrupted (Ctrl+c).
 * Fix `pvxs::client::Subscription::shared_from_this()` leaking internal reference.
 * Fix SharedPV potential race conditions involving "current" Value.

* Changes

 * Ignore beacons with protocol field other than "tcp".  Forward compatibility.
 * Limit packet hex dumps to 64 bytes.
 * ``testStrMatch()`` now specified POSIX regular expression syntax.
 * Client operations builders ``rawRequest(Value())`` is now a no-op.
   Previously produced a non-nonsensical empty request.

* Additions

 * Add `pvxs::client::Context::fromEnv()`.

0.2.0 (July 2021)
-----------------

* Bug fixes

 * Resolve ambiguity between Value::as(T&) and Value::as(FN&&) causing issue with GCC 4.8.
 * Fix encoding of (Sub)Struct w/ valid set.
 * Fix locking issue with client tracking of server beacons.
 * Fix binding to specific interface addresses.

* Changes

 * To simplify usage in situations with complex threading, many client methods avoid unnecessary
   synchronization with the client worker thread.
   Cancellation still synchronizes by default, but this may now be controlled with
   the new syncCancel() Builder methods.  cf. `pvxs::client::detail::CommonBuilder::syncCancel()`.
 * Client Op Builder server() method now implemented.
 * Client channel cache now periodically prunes unused Channels automatically.

* Additions

 * Add server ignore address list.  cf. `pvxs::server::Config::ignoreAddrs`.  Configured from $EPICS_PVAS_IGNORE_ADDR_LIST.
 * Allow TCP timeout to be configured.
 * Add `pvxs::client::Context::connect()` to force Channel creation and retention.
 * Add `pvxs::client::Subscription::shared_from_this()` which can be used with eg. the new `pvxs::MPMCFIFO` to create a work queue.
 * Add `pvxs::server::ExecOp::pvRequest()`
 * Semi-public :ref:`expertapi`.
 * Update bundled libevent
 * Preliminary support for RTEMS 5.1 with libbsd

0.1.5 (May 2021)
----------------

* Bug Fixes

 * Fix several previously unusable template methods of `pvxs::shared_array`
 * Fix `pvxs::logger_level_set`

* Changes

 * Default logger level changed from Err to Warn.
 * Server warns when falling back from requested TCP port.
 * Public headers include <iosfwd> instead of <ostream>.

* Additions

 * Add `pvxs::nt::NTEnum`

0.1.4 (APR 2021)
----------------

* Bug Fixes

 * Fix potential locking issue monitor queue
 * Fix potential locking issue in Shared PV with concurrent open()/close() vs. post()

* Additions

 * Add `pvxs::nt::TimeStamp` and `pvxs::nt::Alarm`.

0.1.3 (FEB 2021)
----------------

* Bug Fixes

 * Fix regression from 0.1.2 causing possible crash on targets defining SO_RXQ_OVFL (eg. Linux).

0.1.2 (FEB 2021)
----------------

* Bug Fixes

 * Fix TCP connection "stall" (incorrect deferred read).

* Changes

 * Raise UDP search reply processing limit.
 * Try not to fragment UDP search packets.
 * mailbox example can serve more than one PV.
 * Indent printed field=value in delta output mode.

0.1.1 (Jan 2021)
----------------

* Bug Fixes

 * Fix decoding of "null" string.  Addresses interoperability with pvAccessJava.
 * Increase inactivity timeout for TCP connections from 30 to 40 seconds.  Also for interoperability with pvAccessJava.
 * Client search requests incorrectly set Server direction bit.  (existing servers don't enforce this)
 * Improved handling of errors resulting from pvRequest processing.  eg. field selection which doesn't select any fields.

* Added Features

 * Added `PVXS_ABI_VERSION` et al. to *pvxs/version.h*
 * Add `testThrowsMatch` and `testStrMatch` to *pvxs/unitttest.h*

* Changes

 * Changed name of automatic Sources ``"builtin"`` and ``"server"`` to ``"__builtin"`` and ``"__server"``.
   Document that Source names beginning with `__` are reserved.

0.1.0 (Dec 2020)
----------------

 * Initial Release
