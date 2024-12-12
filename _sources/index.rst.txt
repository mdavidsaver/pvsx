PVXS client/server for PVA Protocol
===================================

This module provides a library (libpvxs.so or pvxs.dll) and a set of
CLI utilities acting as PVAccess protocol client and/or server.

PVXS is functionally equivalent to the
`pvDataCPP <https://github.com/epics-base/pvDataCPP>`_ and
`pvAccessCPP <https://github.com/epics-base/pvAccessCPP>`_ modules,
which it hopes to eventually supplant (Ok, the author hopes).

- VCS: https://github.com/epics-base/pvxs
- Docs: https://epics-base.github.io/pvxs
- `Issues <https://github.com/epics-base/pvxs/issues>`_ (see :ref:`reportbug`)
- :ref:`contrib`

Dependencies

* A C++11 compliant compiler

 * GCC >= 4.8
 * Visual Studio >= 2015 / 12.0'
 * clang

* `EPICS Base <https://epics-controls.org/resources-and-support/base/>`_ >=3.15.1
* `libevent <http://libevent.org/>`_ >=2.0.1  (Optionally bundled)
* (optional) `CMake <https://cmake.org/>`_ >=3.1, only needed when building bundled libevent

See :ref:`building` for details.

Download
--------

Releases are published to https://github.com/epics-base/pvxs/releases.
See :ref:`relpolicy` for details.

.. toctree::
   :maxdepth: 3
   :caption: Contents:

   overview
   netconfig
   example
   building
   cli
   value
   client
   server
   ioc
   util
   details
   releasenotes


Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
