.. _ntapi:

Common Type Definitions
=======================

Library of common type definitions. ::

    #include <pvxs/nt.h>
    namespace pvxs { namespace nt { ... } }

.. toctree::
   :maxdepth: 1
   :caption: Contents:

   ntscalar

.. note::
    The nt::* factories are relatively expensive.
    Call them once, then use `pvxs::Value::cloneEmpty` on the result.

.. _ntcompat:

Forward Compatibility
---------------------

The `Value` objects returned by the ``NT*`` type builders are considered part of the API
for the purposes of the :ref:`relpolicy`.
Addition of a field is considered a compatible change.
An incompatible change being:

- Removal of a structure field
- A field type change which restricts allowed assignments.
  eg. changing ``int32_t`` -> ``string`` would be compatible, but ``string`` -> ``int32_t`` would not.

The status of change to a struct ID string are currently undefined with respect to compatibility
as the consequences have not yet been explored in practice.

.. _time_t:

time_t
------

Commonly used sub-structure to represent a time

.. doxygenstruct:: pvxs::nt::TimeStamp
    :members:

**"secondsPastEpoch"**
    Seconds since POSIX epoch of 1 Jan 1970 UTC.
    Note that the EPICS epoch is 631152000 seconds after the POSIX epoch.
    (cf. POSIX_TIME_AT_EPICS_EPOCH in epicsTime.h from EPICS Base)

**"nanoseconds"**
    Number of nanoseconds since the start of the second.

**"timeStamp.userTag"**
    Extra site-specific information which may provide context for the seconds/nanoseconds value.

.. _alarm_t:

alarm_t
-------

.. doxygenstruct:: pvxs::nt::Alarm
    :members:

**"severity"**
    Enumeration of 0 - No Alarm, 1 - Minor, 2 - Major, 3 - Invalid.

    The meaning of Minor and Major are contextual and may be different for each PV.
    An Invalid alarm severity means that the value field should not be taken as
    a meaningful representation of eg. the quantity being measured.  Typically,
    it reflects the most recent valid value.

    A UI client may change the state of a widget displaying a value which is alarming
    by eg. changing border color.

**"status"**
    Enumeration providing context to **"severity"** 0 - No Alarm, 1 - Device, 2 - Driver,
    3 - Record, 4 - Database, 5 - Configuration, 6 - Undefined, 7 - Client

**"message"**
    Arbitrary string describing the condition being alarmed.

NTEnum
------

Container for a scalar value selection from a list of strings.

.. doxygenstruct:: pvxs::nt::NTEnum
    :members:

NTNDArray
---------

Container for image data used by areaDetector.

.. doxygenstruct:: pvxs::nt::NTNDArray
    :members:

NTTable
-------

Container for tabular data.

.. doxygenstruct:: pvxs::nt::NTTable
    :members:

NTURI
-----

.. doxygenclass:: pvxs::nt::NTURI
    :members:
