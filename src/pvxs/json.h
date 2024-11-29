/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef PVXS_JSON_H
#define PVXS_JSON_H

#include <string>

#include <stdlib.h>
#include <string.h>

#include <pvxs/version.h>

namespace pvxs {
class Value;

namespace json {

/** Parse JSON string
 *
 *  A character array must remain valid and unmodified for
 *  the lifetime of the Parse object referencing it.
 *
 *  @since UNRELEASED
 */
struct Parse {
    const char* base;
    size_t count;

    //! Nil terminated string.
    inline
    Parse(const char *s)
        :base(s)
        ,count(strlen(s))
    {}
    //! Character array without terminating Nil.
    inline
    constexpr Parse(const char *s, size_t c)
        :base(s)
        ,count(c)
    {}
    //! String
    inline
    Parse(const std::string& s)
        :base(s.c_str())
        ,count(s.size())
    {}

    /** Assign previously created Value from parsed string.
     *
     *  Provided Value may be modified on error.
     *
     *  @pre v.valid()
     */
    PVXS_API
    void into(Value& v) const;

    //! Infer type from JSON value
    PVXS_API
    Value as() const;
};

} // namespace json
} // namespace pvxs

#endif // PVXS_JSON_H
