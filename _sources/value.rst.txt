Value Container API
===================

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   nt
   typedef

.. code-block:: c++

    #include <pvxs/data.h>
    namespace pvxs { ... }

`pvxs::Value` is the primary data container type used with PVXS.
A `pvxs::Value` may be obtained via the remote peer (client or server),
or created locally.  See `ntapi` or `typedefapi`.

`pvxs::Value` is a safe pointer-like object which, maybe, references
a node in a tree of sub-structures and leaf fields.
This tree will be referred to as a Structure as it behaves
in many ways like a C 'struct'.

For example, the following code:

.. code-block:: c++

    Value top = TypeDef(TypeCode::Struct, {
        members::Int32("fldname"),
    }).create();

    top["fldname"] = 1;

    fld = top["fldname"];
    fld = 2;

Is analogous to the following pseudo code.

.. code-block:: c++

    // pseudo-code
    struct anon {
        int32_t fldname=0u;
    };
    void* top = new anon;

    static_cast<anon*>(top)->fldname = 1;

    void* fld = &static_cast<anon*>(top)->fldname;
    static_cast<int32_t*>(fld) = 2;

With the chief functional difference being that the analogs of the casts are made safe.
Also, the storage of the underlying Structure will be free'd when no more Values reference it.

A Value which does not reference any underlying Structure is not valid, or "empty".

.. code-block:: c++

    Value dummy;
    assert(!dummy.valid());
    assert(!dummy); // operator bool() is an alias for valid()

An invalid Value may be returned on error by some methods.
All operations on an invalid Value should be safe and well defined.

.. code-block:: c++

    Value top(nt::NTScalar{TypeCode::Int32}.create());
    int32_t val = top["nonexistent"].as<int32_t>();

In this example, the operator[] lookup of a non-existent field returns an invalid Value.
Attempting to extract an integer from this will then throw a `pvxs::NoField` exception.

Value
-----

Field Lookup
^^^^^^^^^^^^

Access to members of structured types is accomplished through `pvxs::Value::operator[]` or `pvxs::Value::lookup`.
These two methods differ in how errors are communicated.
operator[] will return an "invalid" or "empty" Value if the expression does not address a member.
lookup() will throw an exception describing where and how expression evaluation failed.

Iteration
^^^^^^^^^

`pvxs::Value` instances pointing to a non-array structured data field (Struct or Union)
may be iterated.  Iteration comes in three variations: `pvxs::Value::iall`, `pvxs::Value::ichildren`,
and `pvxs::Value::imarked`.

For a Struct, iall() is a depth first traversal of all fields.
ichildren() traverses all child fields (excluding eg. grandchildren
and further).  imarked() considers all fields, but only visits
those which have beem marked (`pvxs::Value::isMarked`).

For a Union.  iall() and ichildren() are identical, and will
visit all possible Union members, excluding the implicit NULL member.
Traversal does not effect member selection.
imarked() for a Union will visit at most one member (if one is selected)>

Iteration of Union may return Value instances
allocated with temporary storage.  Changes to these instances
will not effect the underlying structure.

Iteration of other field types, including StructA and UnionA is not implemented at this time,
and will always appear as empty.

.. doxygenclass:: pvxs::Value
    :members:

.. doxygenstruct:: pvxs::NoField

.. doxygenstruct:: pvxs::NoConvert

.. doxygenstruct:: pvxs::LookupError

Array fields
------------

Array fields are represented with the `pvxs::shared_array` container
using void vs. non-void, and const vs. non-const element types.

Arrays are initially created as non-const and non-void.
After being populated, an array must be transformed using
`pvxs::shared_array::freeze` to become const before
being stored in a `pvxs::Value`.

.. code-block:: c++

    shared_array<double> arr({1.0, 2.0});
    Value top = nt::NTScalar{TypeCode::Float64A}.create();

    top["value"] = arr.freeze();
    # freeze() acts like std::move().  arr is now empty
    # only the read-only reference remains!

The `pvxs::shared_array::freeze` method is special in that it
acts like std::move() in that it moves the array reference into the returned object.
freeze() requires exclusive ownership of the reference being frozen.
An exception will be thrown unless `pvxs::shared_array::unique` would return true.

Array values may be extracted from `pvxs::Value` as either const void or const non-void.
The const non-void option is a convenience which may **allocate** and do an element by element conversion.

.. code-block:: c++

    # extract reference, or converted copy
    arr = top["value"].as<shared_array<const double>>();

When it is desirable to avoid an implicit allocate and convert,
an array can be extracted as "const void".
This does require calling `pvxs::shared_array::original_type` to find the `pvxs::ArrayType`
of the underlying array prior to using `pvxs::shared_array::castTo`.

.. code-block:: c++

    # extract untyped reference.  Never copies
    shared_array<const void> varr = top["value"].as<shared_array<const void>>();
    if(varr.original_type()==ArrayType::Float64) {
        # castTo() throws std::logic_error if the underlying type is not 'double'.
        shared_array<const double> temp = varr.castTo<const double>();
    }

.. doxygenclass:: pvxs::shared_array
    :members:

.. doxygenfunction:: pvxs::elementSize

.. doxygenclass:: pvxs::detail::Limiter
    :members:

.. doxygenenum:: pvxs::ArrayType
