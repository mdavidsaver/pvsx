.. _typedefapi:

Defining Custom Types
=====================

Interface through which type definitions are created and extended for use with `pvxs::Value`. ::

    #include <pvxs/data.h>
    namespace pvxs { ... }

The type system of the PVA protocol consists of a number of primitive and compound types,
as well as arrays of either.
The `pvxs::TypeCode::code_t` enum is an exhaustive list of all valid types,
as well as Null.

Any valid type may be instantiate directly.

.. code-block:: c++

    Value v = TypeDef{TypeCode::String}.create();
    v = "works, but not very interesting";

In almost all cases, custom type definitions will be for Structures.
When defining structures, the functions of the `pvxs::members` namespace can help to keep definitions concise.

.. code-block:: c++

    namespace M = pvxs::members;

    Value v = TypeDef(TypeCode::Struct, {
        M::Int32("ifield"),
        M::Float64("fval"),
        M::Struct("substruct", {
            M::String("desc"),
        }),
    }).create();

It is also possible to extend definitions.

.. code-block:: c++

    namespace M = pvxs::members;

    TypeDef def(TypeCode::Struct, {
        M::Int32("ifield"),
        M::Struct("substruct", {
            M::String("desc"),
        }),
    });

    def += {M::Float64("fval")};
    Value v(def.create());

This also applies to the `ntapi` allowing fields to be added.
eg. adding a string field "display.notes".

.. code-block:: c++

    namespace M = pvxs::members;

    TypeDef def = nt::NTScalar{TypeCode::UInt32, true}.build();

    def += {M::Struct("display", {M::String("notes")})};
    Value v(def.create());

.. doxygenstruct:: pvxs::TypeCode
    :members:

.. doxygenclass:: pvxs::TypeDef
    :members:

.. doxygenstruct:: pvxs::Member
    :members:

.. doxygennamespace:: pvxs::members
    :members:

.. doxygenenum:: pvxs::Kind
