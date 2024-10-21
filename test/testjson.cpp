/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <stdexcept>

#include <testMain.h>

#include <dbDefs.h>
#include <epicsUnitTest.h>

#include <pvxs/unittest.h>
#include <pvxs/json.h>
#include <pvxs/nt.h>
#include <pvxs/data.h>

#include "utilpvt.h"

using namespace pvxs;
namespace  {

void testBad()
{
    testDiag("%s", __func__);

    static const char* inputs[] = {
        "",
        " 14 x",
        " {",
        " {} extra",
        R"({"value":{"A":[1,2], "B":"[1.5, 2.5]}}")",
    };

    for(size_t i=0; i<NELEMENTS(inputs); i++) {
        auto inp = inputs[i];
        testThrows<std::runtime_error>([inp](){
            Value empty;
            json::Parse(inp).into(empty);
        })<<" Expected error from "<<escape(inp);
    }
}

void testScalar()
{
    testDiag("%s", __func__);

    {
        Value val(TypeDef(TypeCode::String).create());
        json::Parse(R"( "hello" )").into(val);
        testEq(val.as<std::string>(), "hello");
    }

    {
        Value val(TypeDef(TypeCode::Int16).create());
        json::Parse("42").into(val);
        testEq(val.as<int16_t>(), 42);
    }

    {
        Value val(TypeDef(TypeCode::Int64).create());
        json::Parse(" -42  ").into(val);
        testEq(val.as<int16_t>(), -42);
    }

    {
        Value val(TypeDef(TypeCode::Float64).create());
        json::Parse(" -42.5 ").into(val);
        testEq(val.as<double>(), -42.5);
    }
}

void testStruct()
{
    testDiag("%s", __func__);

    {
        Value top(nt::NTScalar{TypeCode::UInt16, true}.create());
        json::Parse(R"( {"value":43, "alarm":{"severity":1, "status":2, "message":"hello"}, "display":{}} )").into(top);
        testEq(top["value"].as<int16_t>(), 43);
        testEq(top["alarm.severity"].as<int16_t>(), 1);
        testEq(top["alarm.status"].as<int16_t>(), 2);
        testEq(top["alarm.message"].as<std::string>(), "hello");
    }
}

void testArrayOfScalar()
{
    testDiag("%s", __func__);

    {
        Value val(TypeDef(TypeCode::Int32A).create());
        json::Parse(R"( [1, 2,3 ] )").into(val);
        shared_array<const int32_t> expect({1, 2, 3});
        testArrEq(val.as<shared_array<const int32_t>>(), expect);
    }

    {
        Value val(TypeDef(TypeCode::Int32A).create());
        json::Parse(R"( [1, 2.5,3 ] )").into(val);
        shared_array<const int32_t> expect({1, 2, 3});
        testArrEq(val.as<shared_array<const int32_t>>(), expect);
    }

    {
        Value val(TypeDef(TypeCode::Float64A).create());
        json::Parse(R"( [1.5, 2,3 ] )").into(val);
        shared_array<const double> expect({1.5, 2.0, 3.0});
        testArrEq(val.as<shared_array<const double>>(), expect);
    }

    {
        Value val(TypeDef(TypeCode::StringA).create());
        json::Parse(R"( ["1", "hello", "world" ] )").into(val);
        shared_array<const std::string> expect({"1", "hello", "world"});
        testArrEq(val.as<shared_array<const std::string>>(), expect);
    }
}

void testStructArray()
{
    using namespace pvxs::members;

    testDiag("%s", __func__);

    {
        Value val(TypeDef(TypeCode::StructA, {
                              Int32("ival"),
                              String("sval"),
                              Int32A("aval"),
                          }).create());
        json::Parse("["
                    "{\"ival\":1, \"sval\":\"hello\"},"
                    "null,"
                    "{\"aval\": [4,5,6]}"
                    "]").into(val);
        testStrEq(std::string(SB()<<val),
                  "struct[] = {3}[\n"
                  "    struct {\n"
                  "        int32_t ival = 1\n"
                  "        string sval = \"hello\"\n"
                  "        int32_t[] aval = {?}[]\n"
                  "    }\n"
                  "    null\n"
                  "    struct {\n"
                  "        int32_t ival = 0\n"
                  "        string sval = \"\"\n"
                  "        int32_t[] aval = {3}[4, 5, 6]\n"
                  "    }\n"
                  "]\n");
    }
}

void testUnionArray()
{
    using namespace pvxs::members;

    testDiag("%s", __func__);

    {
        Value val(TypeDef(TypeCode::UnionA, {
                              Int32("ival"),
                              String("sval"),
                              Int32A("aval"),
                          }).create());
        json::Parse("["
                    "{\"ival\":1},"
                    "null,"
                    "{\"aval\": [4,5,6]}"
                    "]").into(val);
        testStrEq(std::string(SB()<<val),
                  "union[] = {3}[\n"
                  "    union.ival int32_t = 1\n"
                  "    null\n"
                  "    union.aval int32_t[] = {3}[4, 5, 6]\n"
                  "]\n");
    }
}

void testNTTable()
{
    auto val(nt::NTTable{}
             .add_column(TypeCode::Float64, "A")
             .add_column(TypeCode::Float64, "B")
             .create());

    json::Parse(R"({"value":{"A":[1,2], "B":[1.5, 2.5]}})").into(val);
    testStrEq(std::string(SB()<<val),
              "struct \"epics:nt/NTTable:1.0\" {\n"
              "    string[] labels = {2}[\"A\", \"B\"]\n"
              "    struct {\n"
              "        double[] A = {2}[1, 2]\n"
              "        double[] B = {2}[1.5, 2.5]\n"
              "    } value\n"
              "    string descriptor = \"\"\n"
              "    struct \"alarm_t\" {\n"
              "        int32_t severity = 0\n"
              "        int32_t status = 0\n"
              "        string message = \"\"\n"
              "    } alarm\n"
              "    struct \"time_t\" {\n"
              "        int64_t secondsPastEpoch = 0\n"
              "        int32_t nanoseconds = 0\n"
              "        int32_t userTag = 0\n"
              "    } timeStamp\n"
              "}\n");
}

} // namespace

MAIN(testjson)
{
    testPlan(20);
    testBad();
    testScalar();
    testStruct();
    testArrayOfScalar();
    testStructArray();
    testUnionArray();
    testNTTable();
    cleanup_for_valgrind();
    return testDone();
}
