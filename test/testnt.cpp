/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <testMain.h>

#include <epicsUnitTest.h>

#include <pvxs/unittest.h>
#include <pvxs/nt.h>

namespace {

using namespace pvxs;

void testNTScalar()
{
    testDiag("In %s", __func__);

    // plain, without display meta
    auto top = nt::NTScalar{TypeCode::Int32}.create();

    testTrue(top.idStartsWith("epics:nt/NTScalar:"))<<"\n"<<top;

    testEq(top["value"].type(), TypeCode::Int32);
    testEq(top["display.limitLow"].type(), TypeCode::Null);
    testEq(top["display.description"].type(), TypeCode::Null);
    testEq(top["control.limitLow"].type(), TypeCode::Null);

    // with display, but not control
    top = nt::NTScalar{TypeCode::Float64,true}.create();

    testEq(top["value"].type(), TypeCode::Float64);
    testEq(top["display.limitLow"].type(), TypeCode::Float64);
    testEq(top["display.description"].type(), TypeCode::String);
    testEq(top["control.limitLow"].type(), TypeCode::Null);

    // with everything
    top = nt::NTScalar{TypeCode::Float64,true,true,true}.create();

    testEq(top["value"].type(), TypeCode::Float64);
    testEq(top["display.limitLow"].type(), TypeCode::Float64);
    testEq(top["display.description"].type(), TypeCode::String);
    testEq(top["control.limitLow"].type(), TypeCode::Float64);
}

void testNTNDArray()
{
    testDiag("In %s", __func__);

    auto top = nt::NTNDArray{}.create();

    testTrue(top.idStartsWith("epics:nt/NTNDArray:"))<<"\n"<<top;
}

void testNTURI()
{
    testDiag("In %s", __func__);

    using namespace pvxs::members;

    auto def = nt::NTURI({
                             UInt32("arg1"),
                             String("arg2"),
                         });

    auto top = def.call(42, "hello");

    testTrue(top.idStartsWith("epics:nt/NTURI:"))<<"\n"<<top;
    testEq(top["query.arg1"].as<uint32_t>(), 42u);
    testEq(top["query.arg2"].as<std::string>(), "hello");
}

void testNTEnum()
{
    testDiag("In %s", __func__);

    auto top = nt::NTEnum{}.create();

    testTrue(top.idStartsWith("epics:nt/NTEnum:"))<<"\n"<<top;

    top.lookup("value.index") = 2;
    top.lookup("value.choices") = shared_array<const std::string>({"A", "B", "C"});

    auto value(top["value"]);
    testEq(value.as<std::string>(), "C");
    testEq(value.as<int64_t>(), 2);
    testEq(value.as<uint64_t>(), 2u);

    value = "1";
    testEq(value.as<std::string>(), "B");

    value = "A";
    testEq(value.as<std::string>(), "A");

    value.from<uint64_t>(1u);
    testEq(value.as<std::string>(), "B");

    value.from<uint64_t>(2u);
    testEq(value.as<std::string>(), "C");
}

void testNTTable()
{
    testDiag("In %s", __func__);

    auto top = nt::NTTable{}
            .add_column(TypeCode::Int32, "A", "Col A")
            .add_column(TypeCode::String, "B", "Col B")
            .create();

    shared_array<const std::string> labels({"Col A", "Col B"});
    testArrEq(top["labels"].as<shared_array<const std::string>>(), labels);
    testTrue(top["value.A"].type()==TypeCode::Int32A);
    testTrue(top["value.B"].type()==TypeCode::StringA);
}

} // namespace

MAIN(testnt) {
    testPlan(28);
    testNTScalar();
    testNTNDArray();
    testNTURI();
    testNTEnum();
    testNTTable();
    return testDone();
}
