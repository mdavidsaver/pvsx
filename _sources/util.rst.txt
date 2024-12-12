Misc
====

Logging
-------

PVXS internally logs warning/status messages using errlog.h from EPICS Base.
User applications may control which messages are printed, and may add output. ::

    #include <pvxs/log.h>
    namespace pvxs { ... }

Control of log message output is available through named loggers.
All internal logger names begin with the prefix "pvxs.".

In addition to a name, each logger has an associated integer logging level.
A message will be logged if the level of the message is less than or
equal to the level of the associated logger.

To enable all logging at full detail. ::

    export PVXS_LOG="*=DEBUG"

.. doxygenenum:: pvxs::Level

Controlling Logging
^^^^^^^^^^^^^^^^^^^

By default, all loggers have level Err.
It is recommended that user applications prefer configuration
through the environment variable **$PVXS_LOG** by calling `pvxs::logger_config_env`.

.. doxygenfunction:: pvxs::logger_config_env()

If this is undesirable, logger levels may be (reset) manually.

.. doxygenfunction:: pvxs::logger_level_set(const char *, Level)

.. doxygenfunction:: pvxs::logger_level_clear()


Logging from User applications
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

To emit log messages from user code, a new logger should be defined with `DEFINE_LOGGER`
which will be usable within the current translation unit.
It is allowable for multiple loggers to have the same name.

Logger names beginning with "pvxs.*" is reserved for internal usage,
and must not be present in user code.

.. doxygendefine:: DEFINE_LOGGER

.. doxygendefine:: log_crit_printf

.. doxygendefine:: log_err_printf

.. doxygendefine:: log_warn_printf

.. doxygendefine:: log_info_printf

.. doxygendefine:: log_debug_printf

.. doxygendefine:: log_printf

.. doxygenstruct:: pvxs::logger
    :members:

Identification
--------------

Compile time access to PVXS library version information. ::

    #include <pvxs/util.h>
    namespace pvxs { ... }

.. doxygendefine:: PVXS_VERSION

.. doxygendefine:: PVXS_ABI_VERSION

.. doxygendefine:: VERSION_INT

eg. to conditionally compile based on library version. ::

    #if PVXS_VERSION < VERSION_INT(1,2,3,4)
    // enable some compatibility code
    #endif

.. doxygenfunction:: pvxs::version_int

.. doxygenfunction:: pvxs::version_str

.. doxygenfunction:: pvxs::version_abi_int

.. doxygenfunction:: pvxs::version_abi_check

Unit-test helpers
-----------------

Extensions to epicsUnitTest.h ::

    #include <pvxs/unittest.h>
    namespace pvxs { ... }

.. doxygendefine:: testTrue

.. doxygendefine:: testFalse

.. doxygendefine:: testEq

.. doxygendefine:: testNotEq

.. doxygendefine:: testStrEq

.. doxygendefine:: testStrMatch

.. doxygendefine:: testArrEq

.. doxygendefine:: testShow

The testEq() macro and friends expand to a function which returns a `pvxs::testCase` instance
which may be used as a `std::ostream` to append text describing a test. eg. ::

    testEq(1, 1)<<"We really hope this is true.";
    if(testNotEq(1, 2)<<"shouldn't be true") {
        // further conditional tests if 1!=2
    }

.. doxygenfunction:: pvxs::testThrows

.. doxygenfunction:: pvxs::testThrowsMatch

.. doxygenclass:: pvxs::testCase
    :members:

IOC Testing
^^^^^^^^^^^

Additional helpers for testing IOC applications. ::

    #include <pvxs/iochooks.h>
    namespace pvxs { namespace ioc { ... } }

When possible, use of the ``TestIOC`` class is recommended for both
forward and backward compatibility with EPICS Base (>= 3.15.0.1).

.. doxygenclass:: pvxs::ioc::TestIOC


Utilities
---------

Misc. utility code. ::

    #include <pvxs/util.h>
    namespace pvxs { ... }

.. doxygenfunction:: pvxs::escape(const std::string&)

.. doxygenfunction:: pvxs::escape(const char *)

.. doxygenfunction:: pvxs::escape(const char *, size_t)

.. doxygenfunction:: pvxs::cleanup_for_valgrind

.. doxygenclass:: pvxs::SigInt

.. doxygenfunction:: pvxs::target_information

.. doxygenclass:: pvxs::MPMCFIFO
    :members:
