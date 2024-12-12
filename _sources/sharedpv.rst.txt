SharedPV and StaticSource
=========================

A SharedPV is a single data value which may be accessed by multiple clients through a Server.
It is "shared" in the sense that all clients are manipulating a single variable.

.. code-block:: c++

    #include <pvxs/sharedpv.h>
    namespace pvxs { namespace server { ... } }

Each SharedPV instance has (after open() ) an associated data structure
which is later manipulated with post().

A simple usage is:

.. code-block:: c++

    using namespace pvxs;

    auto initial = nt::NTScalar{TypeCode::Float64}.create();
    initial["value"] = 42.0;

    auto src(server::StaticSource::build());

    auto pv(server::SharedPV::buildMailbox());
    pv.open(initial);

    src.add(argv[1], pv);

    auto serv = server::Server::Config::fromEnv()
            .build()
            .addSource("box", src.source());

    serv.run();

In this context "mailbox" refers to the default onPut() handler, which simply post()s whatever
the client sends.

An example of a SharedPV with a custom Put handler

.. code-block:: c++

    auto pv(server::SharedPV::buildMailbox());

    pv.opPut([](server::SharedPV& pv,
                std::unique_ptr<server::ExecOp>&& op,
                Value&& top)
    {
        // We decide that .value will be present with .open()
        auto val = top["value"];
        // is client trying to change .value ?
        if(val.isMarked(true, true)) {
            auto val = top["value"].as<double>();

            // clip to range [0, 10]
            top["value"] = std::max(0.0, std::min(val, 10.0));
        }

        // update and send to subscribers
        pv.post(std::move(top));
        // notify client of success  (or call op->error() if not)
        op->reply(); 
    });

    auto initial = nt::NTScalar{TypeCode::Float64}.create();
    initial["value"] = 42.0;

    auto src(server::StaticSource::build());

    pv.open(initial);

.. note:: SharedPV follows the Source rules for :ref:`sourcethreading` and locking.

.. doxygenstruct:: pvxs::server::SharedPV
    :members:

.. doxygenstruct:: pvxs::server::StaticSource
    :members:
