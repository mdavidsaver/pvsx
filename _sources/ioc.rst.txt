IOC Integration
###############

.. code-block:: c++

    #include <pvxs/iochooks.h>
    namespace pvxs { namespace namespace ioc { ... } }

The separate ``pvxsIoc`` library exists to run a PVXS server as part of an IOC.
See also :ref:`includepvxs`.

IOC Integration respects the **$PVXS_LOG** as well as the **$EPICS_PVA\*** environment variables.
Changes to this environment variable are possible prior to
calling ``*_registerRecordDeviceDriver(pdbbase)``.

IOC shell
^^^^^^^^^

The ``pvxsIoc`` library adds several IOC shell functions which apply to all PVs
served by the Integrated PVA server.

.. cpp:function:: void pvxsr(int level)

    PVXS Server Report.  Shows information about server configuration (level==0)
    or about connected clients (level>0).  Indirectly calls `pvxs::server::Source::show`.

.. cpp:function:: void pvxsl(int level)

    PVXS Server List.  Lists attached Sources and PV names.
    Indirectly calls `pvxs::server::Source::onList`.

.. cpp:function:: void pvxsi()

    Print information about module versions, target, and toolchain.
    May be requested when reporting a bug.

Adding custom PVs to Server
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. doxygenfunction:: pvxs::ioc::server

.. _qsrv2:

QSRV 2
######

Beginning with PVXS 1.2.0 the functionality of `QSRV <https://epics-base.github.io/pva2pva>`_
is replicated in the ``pvxsIoc`` library.
As of 1.3.0 this feature preview is considered **beta** level with equivalent functionality.

It is recommended not to load both ``pvxsIoc.dbd`` and ``qsrv.dbd`` in the same IOC process.
However, if this is done.  Users may opt out at runtime by setting
``$PVXS_QSRV_ENABLE=NO`` before ``iocInit()``. ::

    # Default with PVXS >= 1.3.0
    # Needed with PVXS 1.2.x
    epicsEnvSet("PVXS_QSRV_ENABLE", "YES")
    iocInit()

Functionality
-------------

The features of QSRV are divided into three broad categories.

- Single PV access
- Group PV access
- PVA Links


IOC Shell
^^^^^^^^^

IOC Shell commands specific to database integration.

.. cpp:function:: void pvxgl(int level, const char* pattern)

    Group PV information.  At detail level 0, lists Group names.
    Pattern restricts listing to only matching names.

.. cpp:function:: void dbLoadGroup(const char *file, const char* macros)

    Load Group definitions from a separate JSON file.
    (as opposed to ``info(Q:group, {...})`` in a .db file)
    See :ref:`groupjson`.

Single PV
^^^^^^^^^

When QSRV is enabled, access to individual/single PVs in the global process database
is automatic and equivalent to the access provided by the Channel Access server
in the IOC (aka. RSRV).

So ``caget pv:name`` and ``pvxget pv:name`` should be functionally equivalent.

Beginning with 1.2.3, long string detection is automatic in some cases.
eg. ``.NAME`` and ``.INP``.
In some situations adding a ``$`` suffix is still necessary when addressing
a ``DBF_STRING`` or ``DBF_*LINK`` field to make it visible as a PVA string.
It will not be necessary for clients to interpret a ``char[]`` as a "long string". ::

    # eg.
    pvget some:record.NAME$
    pvget some:record.INP$

An ``info(Q:form, "...")`` may be used to set the ``display.form`` PVA meta-data hint
which is used by some OPI clients. ::

    record(longin, "my:bits") {
        field(VAL, "0x1234")
        info(Q:form, "Hex") # hint to clients to render as hexadecimal
    }

or: ::

    record(waveform, "my:long:string") {
        field(FTVL, "CHAR")
        field(NELM, "1024")
        info(Q:form, "String") # hint to QSRV to expose char[] as string
    }

Currently supported format hints are:

- Default
- String
- Binary
- Decimal
- Hex
- Exponential
- Engineering

Group PV
^^^^^^^^

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   qgroup

Access Security
^^^^^^^^^^^^^^^

QSRV will enforce an optional access control policy file (``.acf``) loaded by ``asSetFilename()``.
This policy is applied to Single PVs just as RSRV does for Channel Access.
With Group PVs, restrictions are not defined for the group, but rather for the individual member records.
The same policy applies whether record is accessed individually, or through a group.

Policy application differs from CA (RSRV) in several ways:

Client hostname is always the numeric IP address.
``HAG()`` entries must either contained numeric IP addresses,
or that ``asCheckClientIP=1`` flag must be set to translate hostnames into IPs on ACF file load (effects CA server as well).
This prevents clients from trivially forging "hostname".
In additional to client usernames ``UAG()`` definitions may contained items beginning with ``role/`` which are matched against the list of local systems groups of which the client username is a member.
Username to group lookup is done *locally* by QSRV, and depends on IOC host authentication configuration.
Note that this is still based on the client provided username string. ::

    UAG(special) {
        someone, "role/op"
    }

The "special" ``UAG()`` will match CA or PVA clients with the username "someone".
It will also match a PVA client if the "special" account exists locally,
and is a member of the "op" group (supported on POSIX targets and Windows).

PVAccess Links
^^^^^^^^^^^^^^

Since PVXS 1.3.0.

When built against Base >= 7.0.1, support is enabled for PVAccess links
using JSON link support syntax. ::

    record(longin, "tgt") {}
    record(longin, "src") {
        field(INP, {pva:{pv:"tgt"}})
    }

.. note: The "dbjlr" and "dbpvar" IOC shell command provide information about PVA links in a running IOC.

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   pvalink
