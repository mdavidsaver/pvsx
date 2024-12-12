.. _pvalink:

PVA Links
#########

Since 1.3.0

JSON Reference
==============

A PVA link `JSON schema <pvalink-schema-0.json>`_ definition file is available.

Default values for all keys. ::

    record(longin, "tgt") {}
    record(longin, "src") {
        field(INP, {pva:{
            pv:"tgt",
            field:"",
            local:false,
            Q:4,
            pipeline:false,
            proc:none,
            sevr:false,
            time:false,
            monorder:0,
            retry:false,
            always:false,
            defer:false
        }})
    }

Certain keys are only relevant with certain link types.

+----------+-------+--------+---------+
| Key      | Input | Output | Forward |
+==========+=======+========+=========+
| pv       |   X   |   X    |    X    |
+----------+-------+--------+---------+
| field    |   X   |   X    |    X    |
+----------+-------+--------+---------+
| local    |   X   |   X    |    X    |
+----------+-------+--------+---------+
| proc     |       |   X    |    X    |
+----------+-------+--------+---------+
| retry    |       |   X    |    X    |
+----------+-------+--------+---------+
| atomic   |   X   |        |         |
+----------+-------+--------+---------+
| defer    |   X   |        |         |
+----------+-------+--------+---------+
| monorder |   X   |        |         |
+----------+-------+--------+---------+
| pipeline |   X   |        |         |
+----------+-------+--------+---------+
| Q        |   X   |        |         |
+----------+-------+--------+---------+
| sevr     |   X   |        |         |
+----------+-------+--------+---------+
| time     |   X   |        |         |
+----------+-------+--------+---------+
| always   |       |        |         |
+----------+-------+--------+---------+

``pv`` names the target PV in full.

``field`` selects a sub-structure of the target PV.
By default ``""`` the top level structure is used.
If the target field is a Structure, then ``.value``
and various NTScalar meta-data fields are used.
Otherwise, the the targeted field is used as the value
without any meta-data.

``local`` set to require that ``pv`` reference a record in the local database.

``Q`` requests a certain subscription queue depth.

``pipeline`` selects per-subscription flow control.

``proc`` may be set to:

- ``null`` (default).  Server selects ``"NPP"`` or ``"PP"`` behavior.
- ``true`` / ``"PP""``.  Requests target recorded be "processed" after value update.
- ``false`` / ``"NPP"``.  Requests only value update.
- ``"CP"`` Subscribes to target PV.  Process this record on each update as well as (dis)connect event.
- ``"CPP"``.  Equivalent to ``"CP"`` if ``SCAN=Passive``.  Otherwise behaves like ``"NPP"``.

``sevr`` controls if and how alarm state of target PV is applied to Maximize Severity.

- ``"NMS"`` (default)  Target PV severity is not considered.
- ``"MS"``  All target PV severities are considered.
- ``"MSI"`` Only ``INVALID`` severity is considered.
- ``"MSS"`` Currently equivalent to ``"MSI"``.

``time`` If ``true`` (not default) and the selected field of the target PV has a ``timeStamp`` sub-structure,
then this time will be written to the record ``TIME`` field.
In almost all cases if will also be necessary to set ``field(TSE, "-2")``.

``retry``.  For an output link, and ``true`` (not default), then the most recent incomplete PUT
will be re-tried when the target PV (re)connects.

``always``.  Ignored.  ``false`` (default).

``atomic``.  When ``true`` (not default) then ``"CP"`` and ``"CPP"`` link processing will
be done all related records locked (cf. ``dbLockMany()``).
If several records with ``atomic:true`` are linked to different structure fields of the same target this PV,
then all records will be locked together and all resulting processing will be atomic.

``monorder`` the relative ordering (increasing) of processing of ``"CP"`` and ``"CPP"`` records linked to the same target PV.

``defer``.  If ``true`` (not default) this output link will only cache the value to be PUT,
but will not issue a network operation.
Can be used to update several structure fields in a single network PUT.

Input Links
===========

Creating a ``pva`` link will result in a subscription being made to the target PV.
Updates will be accumulated in a local cache.
Link processing reads the most recent value/alarm/time from this cache.
While disconnected, a link will read the most recent value/time, with an ``INVALID`` alarm severity. ::

    record(longout, "src") {
        field(INP, {pva:{pv:"target:pv"}})
    }

To trigger record processing when a monitor update arrives,
add the ``proc:"CP"`` modifier. ::

    record(longout, "src") {
        field(INP, {pva:{
            pv:"target:pv",
            proc:"CP"
        }})
    }

Output Links
============

The default behavior of a ``pva`` output link is to write the link value into a local cache,
and then immediately flush that cache and into a network PUT operation.
``defer:true`` allows prevents a link from immediately flushing,
which allows changes to multiple fields to be combined into a single PUT (eg. writing to :ref:`grouppv`).

By default, an output link will write the ``.value`` structure field of the target PV.
Any processing is at the discretion of the server.
(hint, look for ``pp(TRUE)`` or ``pp(FALSE)`` in ``*Record.dbd``) ::

    record(longout, "src") {
        field(OUT, {pva:"target:pv"})
    }

or equivalently: ::

    record(longout, "src") {
        field(OUT, {pva:{
            "pv":"target:pv"
        }})
    }

Additionally, as with DB links, a link may request control of target processing
by setting the ``proc:`` key with ``true`` / ``"PP""``, or the opposite with
``false`` / ``"NPP"``.

Forward Links
=============

A ``pva`` forward link will send an empty PUT request (no field changes) to the target PV with ``proc:true``.
If the target PV is a record, then this is equivalent to a PUT of ``.PROC``.
