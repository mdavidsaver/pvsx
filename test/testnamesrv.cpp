/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <testMain.h>

#include <epicsUnitTest.h>

#include <epicsEvent.h>

#include <pvxs/unittest.h>
#include <pvxs/log.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/source.h>
#include <pvxs/nt.h>
#include "utilpvt.h"

namespace {
using namespace pvxs;

void testNameServer()
{
    testShow()<<__func__;

    auto pv(server::SharedPV::buildReadonly());
    pv.open(nt::NTScalar{TypeCode::UInt32}.create()
            .update("value", 42u));

    auto serv(server::Config::isolated()
              .build()
              .addPV("testpv", pv)
              .start());

    testShow()<<"Server config\n"<<serv.config();

    auto cliconf(serv.clientConfig());
    for(auto& addr : cliconf.addressList)
        cliconf.nameServers.push_back(SB()<<"pva://"<<addr<<':'<<cliconf.tcp_port<<'/');
    cliconf.autoAddrList = false;
    cliconf.addressList.clear();

    auto cli(cliconf.build());

    testShow()<<"Client config\n"<<cli.config();

    epicsEvent update;
    auto mon(cli.monitor("testpv")
             .maskConnected(false)
             .maskDisconnected(false)
             .event([&update](client::Subscription&){
                 testDiag("event");
                 update.signal();
             }).exec());

    auto popExc = [&mon, &update]() -> std::exception_ptr {
        while(true) {
            if(auto var = mon->pop()) {
                throw std::runtime_error(SB()<<" Unexpected update\n"<<var);
            }
            if(!update.wait(500.0))
                throw std::runtime_error(SB()<<" Timeout");
        }
    };

    auto popVal = [&mon, &update]() {
        while(true) {
            try {
                if(auto var = mon->pop()) {
                    testEq(var["value"].as<uint32_t>(), 42u);
                    return;
                }
                if(!update.wait(500.0))
                    throw std::runtime_error(SB()<<" Timeout");
            } catch(std::exception& e) {
                testTrue(false)<<" Unexpected update: "<<e.what();
                return;
            }
        }
    };

    testThrows<client::Connected>([&popExc]() { popExc(); });
    popVal();

    testDiag("Stopping server");
    serv.stop();

    testThrows<client::Disconnect>([&popExc]() { popExc(); });

    testDiag("Restarting server");
    serv.start();

    testThrows<client::Connected>([&popExc]() { popExc(); });
    popVal();
}

} // namespace

MAIN(testnamesrv)
{
    testPlan(5);
    testSetup();
    logger_config_env();
    testNameServer();
    cleanup_for_valgrind();
    return testDone();
}
