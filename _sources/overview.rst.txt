Overview
========

Basics
------

What is EPICS?
^^^^^^^^^^^^^^

`Overview <https://docs.epics-controls.org/en/latest/guides/EPICS_Intro.html>`_

`epics-controls.org <https://epics-controls.org>`_

`APS EPICS site <https://epics.anl.gov>`_

What is PVAccess?
^^^^^^^^^^^^^^^^^

PVAccess is network protocol supporting both request/response,
and publish/subscribe operations.

PVA is closely related to the Channel Access (CA) protocol,
which PVA may work alongside, and is intended to supersede.

Four protocol operations are supported by PVXS.

- Get - Fetch the present value of a PV.
- Put - Change the value of a PV.
- Monitor - Subscribe to changes in the value of a PV.
- RPC - A remote method call.

Get, Put, Monitor, and RPC are to the PVA protocol what GET, PUT, POST are to the HTTP protocol.


What is a PV?
^^^^^^^^^^^^^

In the EPICS world a Process Variable (PV) refers to the idea of a globally addressed data structure.
An EPICS control system is composed of many PVs.
In the millions for large facilities.
The present value of a PV is modified by a combination of remote operations via CA and/or PVA,
and via local processing (eg. values read from local hardware).

A common example of a PV is a measurement value, for example
a temperature measured by a particular sensor (eg. ``mylab:temp1``).
Another example would be an electromechanical relay, which may be opened or closed.
(eg. ``mylab:valve2``)

A PV name is needed when initiating any PVA operation.
eg. with the ``pvx*`` utility executables. ::

  ## fetch current value of both measurement and setting
  $ pvxget mylab:temp1 mylab:valve2
  ...
  ## setup subscription of both.
  $ pvxmonitor mylab:temp1 mylab:valve2
  ...
  Ctrl+c
  ## Request setting change
  $ pvxput mylab:valve2 1
  ...

In the case of the relay, a Get operation would poll the current open/closed state of the relay.
A Monitor operation (subscription) would setup and receive notification when the relay state changes.
A Put operation would be used to command the relay to open or close, or perhaps toggle (the precise meaning of a Put is context dependent).

So the Get, Put, and Monitor operation on a given PV are conventionally operating on a common data structure.
The RPC operation is more arbitrary, and need not have any relationship with a common data structure (eg. the open/closed state of the relay.)

.. note:: In the context of the PVA or CA protocols, a "PV name" is an address string which uniquely identifies a Process Variable.
          All PVA network operations begin with a "PV name" string.

A "PV name" string is to the PVA and CA protocols what a URL is to the HTTP protocol.
The main difference being that while a URL is hierarchical, having a hostname and path string,
a PV name is not.  The namespace of PV names is by default all local IP subnets (broadcast domains).
This can be made more complicated though the specifics of client/server network configuration.

The P4P module provides the ability to run PVA clients (cf. :ref:`clientapi`) and/or servers (cf. :ref:`serverapi`).

PVXS Module
-----------

There are three main components of the PVXS module: data container, network client, and network server.

Structured data is packaged into a `pvxs::Value` container.
In the PVA protocol, excepting the RPC operation, the server side of a network connection will dictate
the specific structure used.

A user of the client API will interact with Value instances of these server specified structures.
Conversely, a user of the server API will need to decide on which data structures to use.

Comparison with pvDataCPP
-------------------------

The data component (`pvxs::Value`) of PVXS corresponds with the `pvDataCPP <https://github.com/epics-base/pvDataCPP>`_ module.
It also incorporates parts of the `normativeTypesCPP <https://github.com/epics-base/normativeTypesCPP>`_ module (cf. `ntapi`).

The most obvious difference in the design of pvData vs. PVXS is that the "class PVField" hierarchy is replaced
with the single `pvxs::Value` class.
This avoids the need for explicit, often unsafe, downcasting (base to derived) within this hierarchy.

Further, handling of PVField instances was always by smart pointer,
opening many possibilities to dereference NULL pointers.
By contrast, Value objects handle this indirection internally.
Operations on a empty (aka. NULL) Value are well-defined,
and are made safe by the type system and exceptions.

Sub-field Lookup
^^^^^^^^^^^^^^^^

Consider the following examples with pvDataCPP.
First, as seen in early code.

.. code-block:: c++

    PVStructurePtr top = ...; // maybe result of a Get operation (assume !NULL)
    PVIntPtr value = top->getSubField<PVInt>("value");
    if(!value)
        throw ...
    int32_t val = value->get();

It is necessary to always remember to check for NULL when looking up sub-fields.
Experience has shown that this is very easy to forget, and the result is a client crash
if eg. the server type changes from PVInt (int32) to PVLong (int64).

This can be improved by using the getSubFieldT() method which throws instead of returning NULL.
Using PVScalar intermediate base class allows opportunistic conversion between scalar types,
and throws when this is not possible (eg. between array and scalar).

.. code-block:: c++

    PVStructurePtr top = ...;
    int32_t val = top->getSubFieldT<PVScalar>("value")->getAs<pvInt>();

With PVXS, the behavior is similar with a more compact syntax.

.. code-block:: c++

    Value top = ...; // maybe result of a Get operation (could be NULL)
    int32_t val = top["value"].as<int32_t>();

Another case to consider is when a client wishes to extract a value from an optional field,
or use a default if the field is not provided.

.. code-block:: c++

    PVStructurePtr top = ...;
    uint32_t lim = 1234u; // default
    if(PVScalarPtr limitHigh = top->getSubField<PVScalar>("display.limitHigh")) {
        lim = limitHigh->getAs<pvUInt>(); // could still throw!
    }

With PVXS

.. code-block:: c++

    Value top = ...;
    uint32_t lim = 1234u; // default
    (void)top["display.limitHigh"].as(lim); // returns true if lim is updated

Structure Iteration
^^^^^^^^^^^^^^^^^^^

Also consider iteration of the fields of a structure (children).

.. code-block:: c++

    PVStructurePtr top = ...;
    for(PVFieldPtr& fld : top->getPVFields()) {
        std::cout<< fld->getFullName() <<" : "<<*fld<<"\n";
    }

With PVXS

.. code-block:: c++

    Value top = ...;
    for(Value fld : top.ichildren()) {
        std::cout<< top.nameOf(fld) <<" : "<<fld<<"\n";
    }

Where **ichildren()** could be replaced with **iall()** for a depth first iteration
of all sub-fields within this structure, with **imarked()** for a depth first iteration
of sub-fields marked as changed.  Which brings us to:

Testing for changed fields
^^^^^^^^^^^^^^^^^^^^^^^^^^

While the PVA protocol is based around the idea of transferring partial updates
to some structure fields, the PVField container classes don't incorporate this.
Instead, it is necessary to handle an separate BitSet object provided alongside each PVStructure.

With PVXS, tracking of changed (or valid) fields is built into the Value class.

For example, completion of a Get operation for a client is notified through the ChannelGetRequester::getDone()
interface.

.. code-block:: c++

    void getDone(const Status& sts,
                 const ChannelGet::shared_pointer op,
                 const PVStructurePtr& top,
                 const BitSet::shared_pointer& valid)
    {
        if(!sts.isSuccess() || !top || !valid) {
            std::cout<<"oops : "<<sts<<"\n";
            return;
        }
        if(PVScalarPtr value = top->getSubField<PVScalar>("value")) {
            if(valid->get(value->getFieldOffset())
               || valid->get(top->getFieldOffset()))
            {
                // "value" exists and is provided
                int32_t val = value->getAs<pvInt>();
            }
        }

To unpack this.  Provided that sts.isSuccess(), and that neither 'top' nor 'valid' are NULL,
the valid bit mask indicates which fields the server has actually provided a value for.
Others retain a local default (zero or empty).

In order to find out if the "value" field has actually been provided by the server,
one must obtain the numeric field offset (bit index) with getFieldOffset(),
and then query the BitSet.

This approach opens the possibility of testing the wrong bit, or more commonly,
not enough bits as it requires explicit knowledge about the PVA concept of "compress" bits
for the top structure and any intermediate sub-structures.

.. code-block:: c++

    [](const pvxs::client::Result&& result)
    {
        try {
            Value top = result(); // throws on local or remote error
            if(Value value = top["value"].ifMarked()) {
                // "value" exists and is provided
                int32_t val = value.as<uint32_t>();
            }
        } catch(std::exception& e) {
            std::cout<<"oops : "<<e.what()<<"\n";
            // also handles local errors
        }

With PVXS, Get completion is notified through an callback functor set with `pvxs::client::GetBuilder::result`,
which will throw an exception if a local or remote error has occurred.

The `pvxs::Value::ifMarked` method allows the lookup and test to be combined.
It is also possible to test separately with the `pvxs::Value::isMarked` method.

Tracking changed fields
^^^^^^^^^^^^^^^^^^^^^^^

A server should perform the complement of this, and keep track of changes
when filling in a structure to be sent.

With PVField et al., this again requires a handling separate BitSet.

.. code-block:: c++

    PVStructurePtr top = ...;
    BitSetPtr changed(new BitSet(top->getNumberFields()));

    PVScalarPtr value = top->getSubFieldT<PVScalar>("value");
    value->putFrom<pvInt>(42);
    changed->set(value->getFieldOffset());

With PVXS Value, this is automatic.

.. code-block:: c++

    Value top = ...;

    top["value"] = 42;
    assert(top["value"].isMarked());

NTScalar
^^^^^^^^

PVXS provides facility for building some common Normative Types, as with the normativeTypesCPP module.

.. code-block:: c++

    PVStructurePtr top = NTScalar::createBuilder()
                        ->value(pvInt)
                        ->addAlarm()
                        ->addTimeStamp()
                        ->addDisplay()
                        ->createPVStructure();

becomes:

.. code-block:: c++

    Value top = nt::NTScalar{Int32, true}.create();

The options are the value type (Int32) and whether display meta-data is included.
Alarm and time meta-data are always included.

Custom Structures
^^^^^^^^^^^^^^^^^

Defining new structures with pvDataCPP is best accomplished with a FieldBuilder.

.. code-block:: c++

    PVStructurePtr top = pvd::getFieldCreate()->createFieldBuilder()
                         ->add("value", pvInt)
                         ->addNestedStructure("alarm")
                             ->add("severity", pvInt)
                         ->endNested()
                         ->createStructure()
                         ->build();

becomes:

.. code-block:: c++

    using namespace pvxs::members;
    Value top = TypeDef(TypeCode::Struct, {
                    Int32("value"),
                    Struct("alarm", {
                        Int32("severity"),
                    }),
                }).create();

One significant difference which may not be immediately obvious is that the later
will be automatically indented correctly by code beautifiers.

Comparison with pvAccessCPP
---------------------------

The client and server components of PVXS are heavily influenced by the `pvac <http://epics-base.github.io/pvAccessCPP/group__pvac.html>`_ and `pvas <http://epics-base.github.io/pvAccessCPP/group__pvas.html>`_ APIs of pvAccessCPP.
eg. the analog of pvac::ClientProvider is `pvxs::client::Context`, while pvas::Server and pvas::SharedPV correspond with `pvxs::server::Server` and `pvxs::server::SharedPV`.

The principle practical difference is that PVXS uses functors where the other APIs using interface classes.

For example, sub-classing pvac::ClientChannel::GetCallback to provide a getDone() callback.

.. code-block:: c++

    struct MyGetCallback : public pvac::ClientChannel::GetCallback {
        pvac::Operation inprog;
        void getDone(const GetEvent& evt) override {
            ...
        }
    };
    ...
    void startOp(ClientChannel& chan, ) {
        MyGetCallback cb;
        cb.inprog = chan.get(&cb);
        ...


With PVXS, this becomes:

.. code-block:: c++

    void startOp(pvxs::client::Context& ctxt) {
        std::shared_ptr<pvxs::Operation> op = ctxt.get("pv:name")
                .result([](pvxs::Result&& result) {
                    ...
                })
                .exec();
        ...
    }
