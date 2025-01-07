/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <atomic>

#include <stdio.h>

#include <testMain.h>
#include <asDbLib.h>
#include <dbAccess.h>
#include <dbLock.h>
#include <epicsTime.h>
#include <epicsExit.h>
#include <generalTimeSup.h>
#include <iocsh.h>

#include "testioc.h"
#include "utilpvt.h"

extern "C" {
extern int testioc_registerRecordDeviceDriver(struct dbBase*);
}

using namespace pvxs;

namespace {

std::atomic<uint32_t> testTimeSec{12345678};

int testTimeCurrent(epicsTimeStamp *pDest)
{
    pDest->secPastEpoch = testTimeSec;
    pDest->nsec = 102030;
    return 0;
}

void checkUTAG(Value& v, int32_t expect, const char *fld="timeStamp.userTag")
{
#ifdef DBR_UTAG
    auto utag = v[fld];
    int32_t tag = -1;
    if(!utag.isMarked() || (tag = utag.as<int32_t>())!=expect)
        testFail("userTag not set (%d != %d)", int(expect), int(tag));
    utag.tryFrom(0);
    utag.unmark();
#endif
}

void testTable()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("tbl:Tbl").exec()->wait(5.0));
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format()),
              "struct \"epics:nt/NTTable:1.0\" {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    struct \"alarm_t\" {\n"
              "        int32_t severity = 3\n"
              "        int32_t status = 2\n"
              "        string message = \"UDF\"\n"
              "    } alarm\n"
              "    struct \"time_t\" {\n"
              "        int64_t secondsPastEpoch = 631152000\n"
              "        int32_t nanoseconds = 0\n"
              "        int32_t userTag = 0\n"
              "    } timeStamp\n"
              "    string[] labels = {2}[\"Column A\", \"Column B\"]\n"
              "    struct {\n"
              "        double[] A = {0}[]\n"
              "        double[] B = {0}[]\n"
              "    } value\n"
              "}\n");
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 3\n"
              "alarm.status int32_t = 2\n"
              "alarm.message string = \"UDF\"\n"
              "timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "timeStamp.nanoseconds int32_t = 0\n"
              "labels string[] = {2}[\"Column A\", \"Column B\"]\n"
              "value.A double[] = {0}[]\n"
              "value.B double[] = {0}[]\n");

    val = ctxt.get("tbl:Tbl").record("atomic",false).exec()->wait(5.0);
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.atomic bool = false\n"
              "alarm.severity int32_t = 3\n"
              "alarm.status int32_t = 2\n"
              "alarm.message string = \"UDF\"\n"
              "timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "timeStamp.nanoseconds int32_t = 0\n"
              "labels string[] = {2}[\"Column A\", \"Column B\"]\n"
              "value.A double[] = {0}[]\n"
              "value.B double[] = {0}[]\n")<<"non-atomic get";

    TestSubscription sub(ctxt.monitor("tbl:Tbl"));
    val = sub.waitForUpdate();
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.queueSize int32_t = 4\n"
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 3\n"
              "alarm.status int32_t = 2\n"
              "alarm.message string = \"UDF\"\n"
              "timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "timeStamp.nanoseconds int32_t = 0\n"
              "labels string[] = {2}[\"Column A\", \"Column B\"]\n"
              "value.A double[] = {0}[]\n"
              "value.B double[] = {0}[]\n");

    testDiag("Update tbl:Tbl");
    shared_array<const double> colA({1.0, 2.0, 3.0});
    shared_array<const double> colB({4.0, 5.0, 6.0});
    testTimeSec++;
    ctxt.put("tbl:Tbl").set("value.A", colA).set("value.B", colB).exec()->wait(5.0);

    val = sub.waitForUpdate();
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format().delta()),
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497679\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "labels string[] = {2}[\"Column A\", \"Column B\"]\n"
              "value.A double[] = {3}[1, 2, 3]\n"
              "value.B double[] = {3}[4, 5, 6]\n");

    testdbGetArrFieldEqual("tbl:A", DBR_DOUBLE, colA.size(), colA.size(), colA.data());
    testdbGetArrFieldEqual("tbl:B", DBR_DOUBLE, colB.size(), colB.size(), colB.data());

    sub.testEmpty();

    val = ctxt.get("tbl2:Tbl").exec()->wait(5.0);
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 3\n"
              "alarm.status int32_t = 2\n"
              "alarm.message string = \"UDF\"\n"
              "timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "timeStamp.nanoseconds int32_t = 0\n"
              "labels string[] = {2}[\"Column B\", \"Column A\"]\n"
              "value.B double[] = {0}[]\n"
              "value.A double[] = {0}[]\n");

}

void testTableOrder()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    const char *orders[][2] = {{"A", "B"}, {"B", "A"}, {"event", "timestamp"}, {"timestamp", "event"}};

    std::string prefix = "tbl:TblOrder:";
    for (auto order: orders) {
        std::string pv = prefix + order[0] + "-" + order[1];
        auto val(ctxt.get(pv.c_str()).exec()->wait(5.0));
        checkUTAG(val, 0);

        unsigned i = 0;
        std::string value_str = "value";
        auto value = val["value"];
        for (auto const &fld: value.ichildren()) {
            testStrEq(value.nameOf(fld), order[i++]);
        }
    }
}

void testEnum()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("enm:ENUM").exec()->wait(5.0));
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format()),
              "struct \"epics:nt/NTEnum:1.0\" {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    struct \"alarm_t\" {\n"
              "        int32_t severity = 0\n"
              "        int32_t status = 0\n"
              "        string message = \"\"\n"
              "    } alarm\n"
              "    struct \"time_t\" {\n"
              "        int64_t secondsPastEpoch = 643497678\n"
              "        int32_t nanoseconds = 102030\n"
              "        int32_t userTag = 0\n"
              "    } timeStamp\n"
              "    struct \"enum_t\" {\n"
              "        string[] choices = {2}[\"ZERO\", \"ONE\"]\n"
              "        int32_t index = 1\n"
              "    } value\n"
              "}\n");
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "value.choices string[] = {2}[\"ZERO\", \"ONE\"]\n"
              "value.index int32_t = 1\n");

    TestSubscription sub(ctxt.monitor("enm:ENUM"));
    val = sub.waitForUpdate();
    checkUTAG(val, 0);
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.queueSize int32_t = 4\n"
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "value.choices string[] = {2}[\"ZERO\", \"ONE\"]\n"
              "value.index int32_t = 1\n");

    testTimeSec++;
    testDiag("Change enm:ENUM to ZERO");
    ctxt.put("enm:ENUM").record("atomic", false).set("value.index", 0).exec()->wait(5.0);
    testdbGetFieldEqual("enm:ENUM:INDEX", DBR_LONG, 0);

    // ntenum.db defines no +trigger so only implied self-trigger.
    // aka. not timeStamp expected
    val = sub.waitForUpdate();
    testStrEq(std::string(SB()<<val.format().delta()),
              "value.index int32_t = 0\n");

    testDiag("attempt to write unwritable choices list");
    {
        testThrows<client::RemoteError>([&ctxt]{
            shared_array<const std::string> choices({"foo"});
            ctxt.put("enm:ENUM").set("value.choices", choices).exec()->wait(5.0);
        });
        const char expect[2][MAX_STRING_SIZE] = {"ZERO", "ONE"};
        testdbGetArrFieldEqual("enm:ENUM:CHOICES", DBR_STRING, 2, 2, expect);
    }

    testDiag("attempt to write both index and choices list");
    {
        shared_array<const std::string> choices({"foo"});
        ctxt.put("enm:ENUM")
                .record("process", false) // no update posted
                .set("value.index", 1)
                .set("value.choices", choices)
                .exec()->wait(5.0);
        const char expect[2][MAX_STRING_SIZE] = {"ZERO", "ONE"};
        testdbGetArrFieldEqual("enm:ENUM:CHOICES", DBR_STRING, 2, 2, expect);
        testdbGetFieldEqual("enm:ENUM:INDEX", DBR_LONG, 1);
    }

    sub.testEmpty();
}

void testImage()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("img:Array").exec()->wait(5.0));
    checkUTAG(val, 0);
    checkUTAG(val, 0, "x.timeStamp.userTag");
    testStrEq(std::string(SB()<<val.format().arrayLimit(5u)),
              "struct \"epics:nt/NTNDArray:1.0\" {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    struct \"alarm_t\" {\n"
              "        int32_t severity = 0\n"
              "        int32_t status = 0\n"
              "        string message = \"\"\n"
              "    } alarm\n"
              "    struct \"time_t\" {\n"
              "        int64_t secondsPastEpoch = 643497678\n"
              "        int32_t nanoseconds = 102030\n"
              "        int32_t userTag = 0\n"
              "    } timeStamp\n"
              "    struct[] attribute = {2}[\n"
              "        struct {\n"
              "            string name = \"ColorMode\"\n"
              "            any value uint16_t = 0\n"
              "            struct \"alarm_t\" {\n"
              "                int32_t severity = 0\n"
              "                int32_t status = 0\n"
              "                string message = \"\"\n"
              "            } alarm\n"
              "            struct \"time_t\" {\n"
              "                int64_t secondsPastEpoch = 0\n"
              "                int32_t nanoseconds = 0\n"
              "                int32_t userTag = 0\n"
              "            } timeStamp\n"
              "        }\n"
              "        struct {\n"
              "            string name = \"\"\n"
              "            any value uint16_t = 0\n"
              "            struct \"alarm_t\" {\n"
              "                int32_t severity = 3\n"
              "                int32_t status = 2\n"
              "                string message = \"UDF\"\n"
              "            } alarm\n"
              "            struct \"time_t\" {\n"
              "                int64_t secondsPastEpoch = 631152000\n"
              "                int32_t nanoseconds = 0\n"
              "                int32_t userTag = 0\n"
              "            } timeStamp\n"
              "        }\n"
              "    ]\n"
              "    struct {\n"
              "        struct \"alarm_t\" {\n"
              "            int32_t severity = 0\n"
              "            int32_t status = 0\n"
              "            string message = \"\"\n"
              "        } alarm\n"
              "        struct \"time_t\" {\n"
              "            int64_t secondsPastEpoch = 643497678\n"
              "            int32_t nanoseconds = 102030\n"
              "            int32_t userTag = 0\n"
              "        } timeStamp\n"
              "    } x\n"
              "    struct[] dimension = {2}[\n"
              "        struct {\n"
              "            int32_t size = 100\n"
              "        }\n"
              "        struct {\n"
              "            int32_t size = 100\n"
              "        }\n"
              "    ]\n"
              "    any value uint16_t[] = {10000}[0, 655, 1310, 1966, 2621, ...]\n"
              "}\n");
    testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)),
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "attribute struct[]\n"
              "attribute[0] struct\n"
              "attribute[0].name string = \"ColorMode\"\n"
              "attribute[0].value any\n"
              "attribute[0].value-> uint16_t = 0\n"
              "attribute[0].alarm.severity int32_t = 0\n"
              "attribute[0].alarm.status int32_t = 0\n"
              "attribute[0].alarm.message string = \"\"\n"
              "attribute[0].timeStamp.secondsPastEpoch int64_t = 0\n"
              "attribute[0].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[0].timeStamp.userTag int32_t = 0\n"
              "attribute[1] struct\n"
              "attribute[1].name string = \"\"\n"
              "attribute[1].value any\n"
              "attribute[1].value-> uint16_t = 0\n"
              "attribute[1].alarm.severity int32_t = 3\n"
              "attribute[1].alarm.status int32_t = 2\n"
              "attribute[1].alarm.message string = \"UDF\"\n"
              "attribute[1].timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "attribute[1].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[1].timeStamp.userTag int32_t = 0\n"
              "x.alarm.severity int32_t = 0\n"
              "x.alarm.status int32_t = 0\n"
              "x.alarm.message string = \"\"\n"
              "x.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "x.timeStamp.nanoseconds int32_t = 102030\n"
              "dimension struct[]\n"
              "dimension[0] struct\n"
              "dimension[0].size int32_t = 100\n"
              "dimension[1] struct\n"
              "dimension[1].size int32_t = 100\n"
              "value any\n"
              "value-> uint16_t[] = {10000}[0, 655, 1310, 1966, 2621, ...]\n");

    TestSubscription sub(ctxt.monitor("img:Array2"));
    val = sub.waitForUpdate();
    checkUTAG(val, 0);
    checkUTAG(val, 0, "x.timeStamp.userTag");
    testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)),
              "record._options.queueSize int32_t = 4\n"
              "record._options.atomic bool = true\n"
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "attribute struct[]\n"
              "attribute[0] struct\n"
              "attribute[0].name string = \"ColorMode\"\n"
              "attribute[0].value any\n"
              "attribute[0].value-> uint16_t = 0\n"
              "attribute[0].alarm.severity int32_t = 0\n"
              "attribute[0].alarm.status int32_t = 0\n"
              "attribute[0].alarm.message string = \"\"\n"
              "attribute[0].timeStamp.secondsPastEpoch int64_t = 0\n"
              "attribute[0].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[0].timeStamp.userTag int32_t = 0\n"
              "attribute[1] struct\n"
              "attribute[1].name string = \"\"\n"
              "attribute[1].value any\n"
              "attribute[1].value-> uint16_t = 0\n"
              "attribute[1].alarm.severity int32_t = 3\n"
              "attribute[1].alarm.status int32_t = 2\n"
              "attribute[1].alarm.message string = \"UDF\"\n"
              "attribute[1].timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "attribute[1].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[1].timeStamp.userTag int32_t = 0\n"
              "value any\n"
              "value-> uint16_t[] = {10000}[0, 655, 1310, 1966, 2621, ...]\n"
              "x.alarm.severity int32_t = 0\n"
              "x.alarm.status int32_t = 0\n"
              "x.alarm.message string = \"\"\n"
              "x.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "x.timeStamp.nanoseconds int32_t = 102030\n"
              "dimension struct[]\n"
              "dimension[0] struct\n"
              "dimension[0].size int32_t = 100\n"
              "dimension[1] struct\n"
              "dimension[1].size int32_t = 100\n");

    testTimeSec++;
    testdbPutFieldOk("img:ArrayData_.PROC", DBR_LONG, 0);

    val = sub.waitForUpdate();
    checkUTAG(val, 0);
    checkUTAG(val, 0, "x.timeStamp.userTag");
    testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)),
              "alarm.severity int32_t = 0\n"
              "alarm.status int32_t = 0\n"
              "alarm.message string = \"\"\n"
              "timeStamp.secondsPastEpoch int64_t = 643497681\n"
              "timeStamp.nanoseconds int32_t = 102030\n"
              "attribute struct[]\n"
              "attribute[0] struct\n"
              "attribute[0].name string = \"ColorMode\"\n"
              "attribute[0].value any\n"
              "attribute[0].value-> uint16_t = 0\n"
              "attribute[0].alarm.severity int32_t = 0\n"
              "attribute[0].alarm.status int32_t = 0\n"
              "attribute[0].alarm.message string = \"\"\n"
              "attribute[0].timeStamp.secondsPastEpoch int64_t = 0\n"
              "attribute[0].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[0].timeStamp.userTag int32_t = 0\n"
              "attribute[1] struct\n"
              "attribute[1].name string = \"\"\n"
              "attribute[1].value any\n"
              "attribute[1].value-> uint16_t = 0\n"
              "attribute[1].alarm.severity int32_t = 3\n"
              "attribute[1].alarm.status int32_t = 2\n"
              "attribute[1].alarm.message string = \"UDF\"\n"
              "attribute[1].timeStamp.secondsPastEpoch int64_t = 631152000\n"
              "attribute[1].timeStamp.nanoseconds int32_t = 0\n"
              "attribute[1].timeStamp.userTag int32_t = 0\n"
              "value any\n"
              "value-> uint16_t[] = {10000}[0, 655, 1310, 1966, 2621, ...]\n"
              "x.alarm.severity int32_t = 0\n"
              "x.alarm.status int32_t = 0\n"
              "x.alarm.message string = \"\"\n"
              "x.timeStamp.secondsPastEpoch int64_t = 643497681\n"
              "x.timeStamp.nanoseconds int32_t = 102030\n"
              "dimension struct[]\n"
              "dimension[0] struct\n"
              "dimension[0].size int32_t = 100\n"
              "dimension[1] struct\n"
              "dimension[1].size int32_t = 100\n");

    sub.testEmpty();

    {
        shared_array<const int32_t> arr({1, 2, 3});
        ctxt.put("img:Array").set("value", arr).pvRequest("record[process=false]").exec()->wait(1111115.0);
        testdbGetArrFieldEqual("img:ArrayData", DBR_LONG, arr.size(), arr.size(), arr.data());
    }
}

void testIQ()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("iq:iq").exec()->wait(5.0));
    checkUTAG(val, 1, "I.timeStamp.userTag");
    checkUTAG(val, 1, "Q.timeStamp.userTag");
    testStrEq(std::string(SB()<<val.format().arrayLimit(5u)),
              "struct {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    struct \"epics:nt/NTScalarArray:1.0\" {\n"
              "        double[] value = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "        struct \"alarm_t\" {\n"
              "            int32_t severity = 0\n"
              "            int32_t status = 0\n"
              "            string message = \"\"\n"
              "        } alarm\n"
              "        struct \"time_t\" {\n"
              "            int64_t secondsPastEpoch = 643497678\n"
              "            int32_t nanoseconds = 102030\n"
              "            int32_t userTag = 0\n"
              "        } timeStamp\n"
              "        struct {\n"
              "            double limitLow = 0\n"
              "            double limitHigh = 0\n"
              "            string description = \"\"\n"
              "            string units = \"\"\n"
              "            int32_t precision = 0\n"
              "            struct \"enum_t\" {\n"
              "                int32_t index = 6\n"
              "                string[] choices = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "            } form\n"
              "        } display\n"
              "        struct {\n"
              "            double limitLow = 0\n"
              "            double limitHigh = 0\n"
              "            double minStep = 0\n"
              "        } control\n"
              "        struct {\n"
              "            bool active = false\n"
              "            double lowAlarmLimit = 0\n"
              "            double lowWarningLimit = 0\n"
              "            double highWarningLimit = 0\n"
              "            double highAlarmLimit = 0\n"
              "            int32_t lowAlarmSeverity = 0\n"
              "            int32_t lowWarningSeverity = 0\n"
              "            int32_t highWarningSeverity = 0\n"
              "            int32_t highAlarmSeverity = 0\n"
              "            double hysteresis = 0\n"
              "        } valueAlarm\n"
              "    } I\n"
              "    struct \"epics:nt/NTScalarArray:1.0\" {\n"
              "        double[] value = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "        struct \"alarm_t\" {\n"
              "            int32_t severity = 0\n"
              "            int32_t status = 0\n"
              "            string message = \"\"\n"
              "        } alarm\n"
              "        struct \"time_t\" {\n"
              "            int64_t secondsPastEpoch = 643497678\n"
              "            int32_t nanoseconds = 102030\n"
              "            int32_t userTag = 0\n"
              "        } timeStamp\n"
              "        struct {\n"
              "            double limitLow = 0\n"
              "            double limitHigh = 0\n"
              "            string description = \"\"\n"
              "            string units = \"\"\n"
              "            int32_t precision = 0\n"
              "            struct \"enum_t\" {\n"
              "                int32_t index = 6\n"
              "                string[] choices = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "            } form\n"
              "        } display\n"
              "        struct {\n"
              "            double limitLow = 0\n"
              "            double limitHigh = 0\n"
              "            double minStep = 0\n"
              "        } control\n"
              "        struct {\n"
              "            bool active = false\n"
              "            double lowAlarmLimit = 0\n"
              "            double lowWarningLimit = 0\n"
              "            double highWarningLimit = 0\n"
              "            double highAlarmLimit = 0\n"
              "            int32_t lowAlarmSeverity = 0\n"
              "            int32_t lowWarningSeverity = 0\n"
              "            int32_t highWarningSeverity = 0\n"
              "            int32_t highAlarmSeverity = 0\n"
              "            double hysteresis = 0\n"
              "        } valueAlarm\n"
              "    } Q\n"
              "    struct {\n"
              "        double i = 1\n"
              "        double q = 1\n"
              "    } phas\n"
              "}\n"
              );
    testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)),
              "record._options.atomic bool = true\n"
              "I.value double[] = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "I.alarm.severity int32_t = 0\n"
              "I.alarm.status int32_t = 0\n"
              "I.alarm.message string = \"\"\n"
              "I.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "I.timeStamp.nanoseconds int32_t = 102030\n"
              "I.display.limitLow double = 0\n"
              "I.display.limitHigh double = 0\n"
              "I.display.description string = \"\"\n"
              "I.display.units string = \"\"\n"
              "I.display.precision int32_t = 0\n"
              "I.display.form.index int32_t = 6\n"
              "I.display.form.choices string[] = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "I.control.limitLow double = 0\n"
              "I.control.limitHigh double = 0\n"
              "Q.value double[] = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "Q.alarm.severity int32_t = 0\n"
              "Q.alarm.status int32_t = 0\n"
              "Q.alarm.message string = \"\"\n"
              "Q.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "Q.timeStamp.nanoseconds int32_t = 102030\n"
              "Q.display.limitLow double = 0\n"
              "Q.display.limitHigh double = 0\n"
              "Q.display.description string = \"\"\n"
              "Q.display.units string = \"\"\n"
              "Q.display.precision int32_t = 0\n"
              "Q.display.form.index int32_t = 6\n"
              "Q.display.form.choices string[] = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "Q.control.limitLow double = 0\n"
              "Q.control.limitHigh double = 0\n"
              "phas.i double = 1\n"
              "phas.q double = 1\n");

    TestSubscription sub(ctxt.monitor("iq:iq"));
    val = sub.waitForUpdate();
    checkUTAG(val, 1, "I.timeStamp.userTag");
    checkUTAG(val, 1, "Q.timeStamp.userTag");
    testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)),
              "record._options.queueSize int32_t = 4\n"
              "record._options.atomic bool = true\n"
              "I.value double[] = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "I.alarm.severity int32_t = 0\n"
              "I.alarm.status int32_t = 0\n"
              "I.alarm.message string = \"\"\n"
              "I.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "I.timeStamp.nanoseconds int32_t = 102030\n"
              "I.display.limitLow double = 0\n"
              "I.display.limitHigh double = 0\n"
              "I.display.description string = \"\"\n"
              "I.display.units string = \"\"\n"
              "I.display.precision int32_t = 0\n"
              "I.display.form.index int32_t = 6\n"
              "I.display.form.choices string[] = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "I.control.limitLow double = 0\n"
              "I.control.limitHigh double = 0\n"
              "Q.value double[] = {500}[0.0174524, 0.0801989, 0.142629, 0.204496, 0.265556, ...]\n"
              "Q.alarm.severity int32_t = 0\n"
              "Q.alarm.status int32_t = 0\n"
              "Q.alarm.message string = \"\"\n"
              "Q.timeStamp.secondsPastEpoch int64_t = 643497678\n"
              "Q.timeStamp.nanoseconds int32_t = 102030\n"
              "Q.display.limitLow double = 0\n"
              "Q.display.limitHigh double = 0\n"
              "Q.display.description string = \"\"\n"
              "Q.display.units string = \"\"\n"
              "Q.display.precision int32_t = 0\n"
              "Q.display.form.index int32_t = 6\n"
              "Q.display.form.choices string[] = {7}[\"Default\", \"String\", \"Binary\", \"Decimal\", \"Hex\", ...]\n"
              "Q.control.limitLow double = 0\n"
              "Q.control.limitHigh double = 0\n"
              "phas.i double = 1\n"
              "phas.q double = 1\n");

    sub.testEmpty();

    testTimeSec++;
    testdbPutFieldOk("iq:Phase:I.PROC", DBR_LONG, 0);

    val = sub.waitForUpdate();
    checkUTAG(val, 2, "I.timeStamp.userTag");
    checkUTAG(val, 2, "Q.timeStamp.userTag");
    {
        auto expected = "I.value double[] = {500}[0.0348995, 0.0975829, 0.159881, 0.221548, 0.282341, ...]\n"
                        "I.alarm.severity int32_t = 0\n"
                        "I.alarm.status int32_t = 0\n"
                        "I.alarm.message string = \"\"\n"
                        "I.timeStamp.secondsPastEpoch int64_t = 643497682\n"
                        "I.timeStamp.nanoseconds int32_t = 102030\n"
                        "Q.value double[] = {500}[0.0348995, 0.0975829, 0.159881, 0.221548, 0.282341, ...]\n"
#if EPICS_VERSION_INT < VERSION_INT(7, 0, 6, 0)
                        "Q.alarm.severity int32_t = 0\n"
                        "Q.alarm.status int32_t = 0\n"
                        "Q.alarm.message string = \"\"\n"
#endif
                        "Q.timeStamp.secondsPastEpoch int64_t = 643497682\n"
                        "Q.timeStamp.nanoseconds int32_t = 102030\n"
                        "phas.i double = 2\n"
                        "phas.q double = 2\n";
        testStrEq(std::string(SB()<<val.format().delta().arrayLimit(5u)), expected);
    }

    sub.testEmpty();
}

void testConst()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("tst:const").exec()->wait(5.0));
    testStrEq(std::string(SB()<<val.format()),
              "struct {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    struct {\n"
              "        double d = 1.5\n"
              "        int64_t i = 14\n"
              "        string s = \"hello\"\n"
              "    } s\n"
              "}\n");
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.atomic bool = true\n"
              "s.d double = 1.5\n"
              "s.i int64_t = 14\n"
              "s.s string = \"hello\"\n"
              );

    TestSubscription sub(ctxt.monitor("tst:const"));
    val = sub.waitForUpdate();
    testStrEq(std::string(SB()<<val.format().delta()),
              "record._options.queueSize int32_t = 4\n"
              "record._options.atomic bool = true\n"
              "s.d double = 1.5\n"
              "s.i int64_t = 14\n"
              "s.s string = \"hello\"\n"
              );
}

void testDbLoadGroup()
{
    testDiag("%s", __func__);
    TestClient ctxt;

    auto val(ctxt.get("tst:fromFile").exec()->wait(5.0));
    testStrEq(std::string(SB()<<val.format()),
              "struct {\n"
              "    struct {\n"
              "        struct {\n"
              "            int32_t queueSize = 0\n"
              "            bool atomic = true\n"
              "        } _options\n"
              "    } record\n"
              "    int64_t value = 3\n"
              "}\n");
}

} // namespace

MAIN(testqgroup)
{
    testPlan(46);
    testSetup();
    {
        generalTimeRegisterCurrentProvider("test", 1, &testTimeCurrent);
        ioc::TestIOC ioc;
        asSetFilename("../testioc.acf");
        testdbReadDatabase("testioc.dbd", nullptr, nullptr);
        testOk1(!testioc_registerRecordDeviceDriver(pdbbase));
        testdbReadDatabase("image.db", nullptr, "N=img");
        ioc::dbLoadGroup("../image.json", "N=img");
        testdbReadDatabase("table.db", nullptr, "N=tbl:,LBL1=Column A,LBL2=Column B,PO1=0,PO2=1");
        testdbReadDatabase("table.db", nullptr, "N=tbl2:,LBL1=Column B,LBL2=Column A,PO1=1,PO2=0");
        testdbReadDatabase("tableorder.db", nullptr, "N=tbl:,VALA=A,VALB=B");
        testdbReadDatabase("tableorder.db", nullptr, "N=tbl:,VALA=B,VALB=A");
        testdbReadDatabase("tableorder.db", nullptr, "N=tbl:,VALA=event,VALB=timestamp");
        testdbReadDatabase("tableorder.db", nullptr, "N=tbl:,VALA=timestamp,VALB=event");
        testdbReadDatabase("ntenum.db", nullptr, "P=enm");
        testdbReadDatabase("iq.db", nullptr, "N=iq:");
        testdbReadDatabase("const.db", nullptr, "P=tst:");
        iocsh("../qgroup.cmd");
        ioc.init();
        testTable();
        testTableOrder();
        testEnum();
        testImage();
        testIQ();
        testConst();
        testDbLoadGroup();
    }
    // call epics atexits explicitly to handle older base w/o de-init hooks
    epicsExitCallAtExits();
    cleanup_for_valgrind();
    return testDone();
}
