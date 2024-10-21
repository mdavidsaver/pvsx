/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <vector>
#include <initializer_list>

#include <string.h>

#include <pvxs/unittest.h>
#include <pvxs/util.h>

#include <testMain.h>
#include <epicsUnitTest.h>
#include <dbDefs.h>

#include "cliutil.h"

namespace pvxs { namespace detail {

template<typename E>
struct test_print<std::vector<E>> {
    template<class C>
    static inline void op(C& strm, const std::vector<E>& v) {
        bool first = true;
        for(auto& e : v) {
            if(first) {
                first = false;
            } else {
                strm<<' ';
            }
            test_print<E>::op(strm, e);
        }
    }
};

template<>
struct test_print<std::pair<char, ArgVal>> {
    template<class C>
    static inline void op(C& strm, const std::pair<char, ArgVal>& v) {
        strm<<'-'<<v.first;
        if(v.second.defined)
            strm<<" "<<pvxs::escape(v.second.value);
    }
};

}} // namespace::detail

MAIN(testcliutil) {
    testPlan(15);

    {
        testDiag("case @%d", __LINE__);
        const char* argv[] = {"exe", "-v", "-a", "Aa", "hello"};
        pvxs::GetOpt opts(NELEMENTS(argv), const_cast<char**>(argv), "a:v");
        testOk(strcmp(opts.argv0, "exe")==0, "%s", opts.argv0);
        decltype (opts.arguments) arguments({{'v', nullptr}, {'a',"Aa"}});
        testArrEq(opts.arguments, arguments);
        decltype (opts.positional) positional({"hello"});
        testArrEq(opts.positional, positional);
    }

    {
        testDiag("case @%d", __LINE__);
        const char* argv[] = {"exe", "-v", "-a", "Aa", "hello"};
        pvxs::GetOpt opts(NELEMENTS(argv), const_cast<char**>(argv), "va:");
        testOk(strcmp(opts.argv0, "exe")==0, "%s", opts.argv0);
        decltype (opts.arguments) arguments({{'v', nullptr}, {'a',"Aa"}});
        testArrEq(opts.arguments, arguments);
        decltype (opts.positional) positional({"hello"});
        testArrEq(opts.positional, positional);
    }

    {
        testDiag("case @%d", __LINE__);
        const char* argv[] = {"exe", "-v", "hello", "-aAa"};
        pvxs::GetOpt opts(NELEMENTS(argv), const_cast<char**>(argv), "va:");
        testOk(strcmp(opts.argv0, "exe")==0, "%s", opts.argv0);
        decltype (opts.arguments) arguments({{'v', nullptr}, {'a',"Aa"}});
        testArrEq(opts.arguments, arguments);
        decltype (opts.positional) positional({"hello"});
        testArrEq(opts.positional, positional);
    }

    {
        testDiag("case @%d", __LINE__);
        const char* argv[] = {"exe", "-v", "hello", "-a"}; // missing value
        pvxs::GetOpt opts(NELEMENTS(argv), const_cast<char**>(argv), "va:");
        testOk(strcmp(opts.argv0, "exe")==0, "%s", opts.argv0);
        decltype (opts.arguments) arguments({{'v', nullptr}, {'?',nullptr}});
        testArrEq(opts.arguments, arguments);
        decltype (opts.positional) positional({"hello"});
        testArrEq(opts.positional, positional);
    }

    {
        testDiag("case @%d", __LINE__);
        const char* argv[] = {"exe", "-vvaTest", "hello"};
        pvxs::GetOpt opts(NELEMENTS(argv), const_cast<char**>(argv), "va:");
        testOk(strcmp(opts.argv0, "exe")==0, "%s", opts.argv0);
        decltype (opts.arguments) arguments({{'v', nullptr}, {'v', nullptr}, {'a', "Test"}});
        testArrEq(opts.arguments, arguments);
        decltype (opts.positional) positional({"hello"});
        testArrEq(opts.positional, positional);
    }

    return testDone();
}
