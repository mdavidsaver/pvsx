Data Source
===========

The low-level API through which a `pvxs::server::Server` may issue requests on behalf of remote clients. :: 

    #include <pvxs/server.h>
    namespace pvxs { namespace server { ... } }

The `pvxs::server::SharedPV` API should be preferred when a Server has a predetermined set of PV names.

The `pvxs::server::Source` interface is a more general, and complex, means of allowing a Server
to respond to PV names as clients search for them.
This may be necessary in specialized cases such as gateway, proxy, or bridge servers.

.. _sourcethreading:

Threading
---------

A Server will invoke user callback functions from one or more internal worker threads.
However, it is guaranteed that callbacks relating to a given PV will never be executed concurrently.
This implies that callbacks for a single operation,
those stored through a `pvxs::server::ChannelControl` and related \*Op,
will also never be executed concurrently.

Ownership and Lifetime
----------------------

The \*Op classes are interfaces through which callback functors are passed.
These functors are stored in underlying, and otherwise hidden, server data structures.
Therefore, it is safe to eg. capture a ``shared_ptr<ExecOp>`` into an ``onCancel``
functor without creating a reference loop.

The lifetime of these server data structures are tied to the remote client.
So variables captured into a functor like `pvxs::server::ConnectOp::onGet`, or onCancel,
will be destroyed when the client times out, closes the channel, or closes the operation.
Also when the server side forces channel closure via `pvxs::server::ConnectOp::close`.
The various \*Close callbacks may also be used if explicit cleanup is needed on
certain conditions.

API
---

.. doxygenstruct:: pvxs::server::Source
    :members:

.. doxygenstruct:: pvxs::server::OpBase
    :members:

.. doxygenstruct:: pvxs::server::ExecOp
    :members:

.. doxygenstruct:: pvxs::server::ConnectOp
    :members:

.. doxygenstruct:: pvxs::server::MonitorControlOp
    :members:

.. doxygenstruct:: pvxs::server::MonitorSetupOp
    :members:

.. doxygenstruct:: pvxs::server::ChannelControl
    :members:

.. doxygenstruct:: pvxs::server::MonitorStat
    :members:

.. doxygenstruct:: pvxs::server::ClientCredentials
    :members:
