
.. _ntscalar:

NTScalar and NTScalarArray
==========================

The ``epics:nt/NTScalar:1.0`` and related ``epics:nt/NTScalarArray:1.0``
definitions describe a primary 'value' and supporting meta-data.
In the case of NTScalarArray the value is a 1-d array of primitive type.
In the case of NTScalar the value is a single primitive value.

Both variants include time and alarm meta-data fields,
and optionally display and control meta-data fields.

.. code-block:: c++

    using namespace pvxs;

    // single integer
    Value iscalar = nt::NTScalar{TypeCode::Int64}.create();

    // eg. access "value" field
    iscalar["value"] = 42;

.. code-block:: c++

    // Functionally equivalent pseudo-C++
    struct NTScalar_Int64 {
        int64_t value;
        struct alarm_t {
            int64_t secondsPastEpoch;
            int32_t nanoseconds;
            int32_t userTag
        } alarm;
        struct time_t {
            int32_t severity;
            int32_t status;
            std::string message
        } timeStamp;
        // if NTScalar::display
        struct display_t {
            int64_t limitLow, limitHigh;
            std::string description, units;
            // if NTScalar::form
            int32_t precision;
            struct enum_t {
                int32_t index;
                std::vector<std::string> choices;
            } form;
        } display;
        // if NTScalar::control
        struct control_t {
            int64_t limitLow, limitHigh;
        } control;
        // ...
    };

    auto iscalar = new NTScalar_Int64(); // not safe!
    iscalar->value = 42;

Fields
------

**"value"**
    The primary field.  May be any `pvxs::TypeCode` other than ``Struct``, ``Union``, ``StructA``, ``UnionA``, or ``Null``.

**"timeStamp.secondsPastEpoch"**,
**"timeStamp.nanoseconds"**,
**"timeStamp.userTag"**

    See common :ref:`time_t`.

**"alarm.severity"**,
**"alarm.status"**,
**"alarm.message"**

    See common :ref:`alarm_t`.

**"display.description"**
    Text providing some context about what this value/PV represents.

**"display.units"**
    Text identifying units of value.  eg. "V" or "Hz"

Meta-data for numeric types.

**"display.limitLow"**

**"display.limitHigh"**
    Hints for clients which can indicate inclusive range of possible values.  eg. a UI gauge widget.
    Ignore unless limitLow < limitHigh

**"control.limitLow"**

**"control.limitHigh"**
    Hints for clients on the inclusive range of values which may reasonably be written to this PV.
    Ignore unless limitLow < limitHigh
    Not authoritative.

**"valueAlarm.lowAlarmLimit"**

**"valueAlarm.highAlarmLimit"**

**"valueAlarm.lowAlarmSeverity"**

**"valueAlarm.highAlarmSeverity"**

**"valueAlarm.lowWarningLimit"**

**"valueAlarm.highWarningLimit"**

**"valueAlarm.lowWarningSeverity"**

**"valueAlarm.highWarningSeverity"**
    Hints for clients on the ranges of values which will result in a alarms of the given severities.
    Could be used by eg. a UI gauge widget to place markers colored by alarm severity.
    Actual alarms are signals with **"alarm.severity"**.

    Two value ranges are defined.  The names Alarm vs. Warning do not have a special significance.

    Ignore \*Alarm\* range unless lowAlarmLimit < highAlarmLimit

    Ignore \*Warning\* range unless lowWarningLimit < highWarningLimit

    If a value is within a range [low\*Limit, high\*Limit] inclusive then no alarm is expected.
    If a value is < low\*Limit then the alarm severity low\*Severity is expected.
    If a value is > high\*Limit then the alarm severity high\*Severity is expected.


Builder API
-----------

.. doxygenstruct:: pvxs::nt::NTScalar
    :members:
