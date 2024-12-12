.. _building:

Building from Source
====================

Begin be fetching all needed source. ::

    git clone --recursive https://github.com/epics-base/pvxs.git
    git clone --branch 7.0 https://github.com/epics-base/epics-base.git

Prepare the PVXS source tree with the location of epics-base: ::

    cat <<EOF > pvxs/configure/RELEASE.local
    EPICS_BASE=\$(TOP)/../epics-base
    EOF

Build Base: ::

    make -C epics-base

Alternatives to install or build libevent >=2.0 .

On RHEL7 and later. ::

    yum install libevent-devel

On RHEL6 and earlier. ::

    yum install libevent2-devel

On Debian/Ubuntu. ::

    apt-get install libevent-dev

To build from source (Requires `CMake <https://cmake.org/>`_): ::

    make -C pvxs/bundle libevent # implies .$(EPICS_HOST_ARCH)

For additional archs: eg. ::

    make -C pvxs/bundle libevent.linux-x86_64-debug

Finally, build PVXS: ::

    make -C pvxs

.. _runtests:

Running Tests
^^^^^^^^^^^^^

It is recommended to run automatic unittests when building a new (to you) version
of PVXS, or building on a new host.  ::

    make -C pvxs runtests

Cross-compiling libevent2
^^^^^^^^^^^^^^^^^^^^^^^^^

The bundled libevent may be built for some cross compile targets.
Currently only cross mingw. ::

    make -C pvxs/bundle libevent.windows-x64-mingw

.. _includepvxs:

Including PVXS in your application
==================================

Including PVXS in an application/IOC using the EPICS Makefiles is straightforward.
Add PVXS to the application configure/RELEASE or RELEASE.local file. ::

    cat <<EOF >> configure/RELEASE.local
    PVXS=/path/to/your/build/of/pvxs
    EPICS_BASE=/path/to/your/build/of/epics-base
    EOF

Then add the pvxs and pvxsIoc libraries as a dependencies to your IOC or support module. eg. ::

    PROD_IOC += myioc
    ...
    myioc_DBD += pvxsIoc.dbd
    ...
    myioc_LIBS += pvxsIoc pvxs
    myioc_LIBS += $(EPICS_BASE_IOC_LIBS)

The "pvxsIoc" library should only be included for IOCs.
It can, and should, be omitted for standalone applications
(eg. GUI clients).

Add the pvxs library as a dependency to your executable or library. eg. ::

    PROD_IOC += myapp
    ...
    myapp_LIBS += pvxs
    myapp_LIBS += Com

libevent will be automatically added for linking.

For those interested, this is accomplished with the logic found in
"cfg/CONFIG_PVXS_MODULE".
