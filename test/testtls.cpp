/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#define PVXS_ENABLE_EXPERT_API

#include <sstream>

#include <string.h>

#include <testMain.h>

#include <epicsUnitTest.h>

#include <pvxs/unittest.h>
#include <pvxs/log.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/source.h>
#include <pvxs/nt.h>

#include "utilpvt.h"

using namespace pvxs;

namespace {

void testGetSuper() {
    testShow()<<__func__;

    auto initial(nt::NTScalar{TypeCode::Int32}.create());
    auto mbox(server::SharedPV::buildReadonly());

    auto serv_conf(server::Config::isolated());
    serv_conf.tls_keychain_file = "superserver1.p12";

    auto serv(serv_conf.build()
              .addPV("mailbox", mbox));

    auto cli_conf(serv.clientConfig());
    cli_conf.tls_keychain_file = "ca.p12";

    auto cli(cli_conf.build());

    mbox.open(initial.update("value", 42));
    serv.start();

    auto conn(cli.connect("mailbox")
              .onConnect([](const client::Connected& c){
        testTrue(c.cred && c.cred->isTLS)<<" Connected with TLS";
    })
              .exec());

    auto reply(cli.get("mailbox").exec()->wait(5.0));
    testEq(reply["value"].as<int32_t>(), 42);
}

void testGetIntermediate() {
    testShow()<<__func__;

    auto initial(nt::NTScalar{TypeCode::Int32}.create());
    auto mbox(server::SharedPV::buildReadonly());

    auto serv_conf(server::Config::isolated());
    serv_conf.tls_keychain_file = "server1.p12";

    auto serv(serv_conf.build()
              .addPV("mailbox", mbox));

    auto cli_conf(serv.clientConfig());
    cli_conf.tls_keychain_file = "ca.p12";

    auto cli(cli_conf.build());

    mbox.open(initial.update("value", 42));
    serv.start();

    auto conn(cli.connect("mailbox")
              .onConnect([](const client::Connected& c){
        testTrue(c.cred && c.cred->isTLS)<<" Connected with TLS";
    })
              .exec());

    auto reply(cli.get("mailbox").exec()->wait(5.0));
    testEq(reply["value"].as<int32_t>(), 42);
}

void testGetNameServer() {
    testShow()<<__func__;

    auto initial(nt::NTScalar{TypeCode::Int32}.create());
    auto mbox(server::SharedPV::buildReadonly());

    auto serv_conf(server::Config::isolated());
    serv_conf.tls_keychain_file = "server1.p12";

    auto serv(serv_conf.build()
              .addPV("mailbox", mbox));

    auto cli_conf(serv.clientConfig());
    cli_conf.tls_keychain_file = "ca.p12";
    for(auto& addr : cli_conf.addressList)
        cli_conf.nameServers.push_back(SB()<<"pvas://"<<addr/*<<':'<<cli_conf.tls_port*/);
    cli_conf.autoAddrList = false;
    cli_conf.addressList.clear();

    auto cli(cli_conf.build());

    mbox.open(initial.update("value", 42));
    serv.start();

    auto conn(cli.connect("mailbox")
              .onConnect([](const client::Connected& c){
        testTrue(c.cred && c.cred->isTLS)<<" Connected with TLS";
    })
              .exec());

    auto reply(cli.get("mailbox")
               .exec()->wait(5.0));
    testEq(reply["value"].as<int32_t>(), 42);
}

struct WhoAmI final : public server::Source {
    const Value resultType;

    WhoAmI()
        :resultType(nt::NTScalar(TypeCode::String).create())
    {}

    virtual void onSearch(Search &op) override final {
        for(auto& pv : op) {
            if(strcmp(pv.name(), "whoami")==0)
                pv.claim();
        }
    }

    virtual void onCreate(std::unique_ptr<server::ChannelControl> &&op) override final {
        if(op->name()!="whoami")
            return;

        op->onOp([this](std::unique_ptr<server::ConnectOp>&& cop) {

            cop->onGet([this](std::unique_ptr<server::ExecOp>&& eop) {
                auto cred(eop->credentials());
                std::ostringstream strm;
                strm<<cred->method<<'/'<<cred->account;

                eop->reply(resultType.cloneEmpty()
                           .update("value", strm.str()));
            });

            cop->connect(resultType);
        });

        std::shared_ptr<server::MonitorControlOp> sub;
        op->onSubscribe([this, sub](std::unique_ptr<server::MonitorSetupOp>&& sop) mutable {
            sub = sop->connect(resultType);
            auto cred(sub->credentials());
            std::ostringstream strm;
            strm<<cred->method<<'/'<<cred->account;

            sub->post(resultType.cloneEmpty()
                      .update("value", strm.str()));
        });
    }
};

Value pop(const std::shared_ptr<client::Subscription>& sub, epicsEvent& evt)
{
    while(true) {
        if(auto ret = sub->pop()) {
            return ret;

        } else if (!evt.wait(5.0)) {
            testFail("timeout waiting for event");
            return Value();
        }
    }
}

void testClientReconfig() {
    testShow()<<__func__;

    auto serv_conf(server::Config::isolated());
    serv_conf.tls_keychain_file = "ioc1.p12";

    auto serv(serv_conf.build()
              .addSource("whoami", std::make_shared<WhoAmI>()));

    auto cli_conf(serv.clientConfig());
    cli_conf.tls_keychain_file = "client1.p12";

    auto cli(cli_conf.build());

    serv.start();

    epicsEvent evt;
    auto sub(cli.monitor("whoami")
             .maskConnected(false)
             .maskDisconnected(false)
             .event([&evt](client::Subscription&) {
                 evt.signal();
             }).exec());
    Value update;

    try {
        pop(sub, evt);
        testFail("Unexpected success");
        testSkip(2, "oops");
    } catch(client::Connected& e) {
        testTrue(e.cred->isTLS);
        testEq(e.cred->method, "x509");
        testEq(e.cred->account, "ioc1");
    }
    testDiag("Connect");

    update = pop(sub, evt);
    testEq(update["value"].as<std::string>(), "x509/client1");

    cli_conf = cli.config();
    cli_conf.tls_keychain_file = "client2.p12;oraclesucks";
    testDiag("cli.reconfigure()");
    cli.reconfigure(cli_conf);

    testThrows<client::Disconnect>([&sub, &evt]{
        pop(sub, evt);
    });
    testDiag("Disconnect");

    try {
        (void)pop(sub, evt);
        testFail("Missing expected Connected");
    }catch(client::Connected& e){
        testOk1(e.cred && e.cred->isTLS);
    }catch(...){
        testFail("Unexpected exception instead of Connected");
    }
    testDiag("Reconnect");

    update = pop(sub, evt);
    testEq(update["value"].as<std::string>(), "x509/client2");
}

void testServerReconfig() {
    testShow()<<__func__;

    auto serv_conf(server::Config::isolated());
    serv_conf.tls_keychain_file = "server1.p12";

    auto serv(serv_conf.build()
              .addSource("whoami", std::make_shared<WhoAmI>()));

    auto cli_conf(serv.clientConfig());
    cli_conf.tls_keychain_file = "ioc1.p12";

    auto cli(cli_conf.build());

    serv.start();

    epicsEvent evt;
    auto sub(cli.monitor("whoami")
             .maskConnected(false)
             .maskDisconnected(false)
             .event([&evt](client::Subscription&) {
                 evt.signal();
             }).exec());
    Value update;

    try {
        pop(sub, evt);
        testFail("Unexpected success");
        testSkip(2, "oops");
    } catch(client::Connected& e) {
        testTrue(e.cred->isTLS);
        testEq(e.cred->method, "x509");
        testEq(e.cred->account, "server1");
    }
    testDiag("Connect");

    update = pop(sub, evt);
    testEq(update["value"].as<std::string>(), "x509/ioc1");

    serv_conf = serv.config();
    serv_conf.tls_keychain_file = "ioc1.p12";
    testDiag("serv.reconfigure()");
    serv.reconfigure(serv_conf);

    testThrows<client::Disconnect>([&sub, &evt]{
        pop(sub, evt);
    });
    testDiag("Disconnect");

    try {
        pop(sub, evt);
        testFail("Unexpected success");
        testSkip(2, "oops");
    } catch(client::Connected& e) {
        testTrue(e.cred->isTLS);
        testEq(e.cred->method, "x509");
        testEq(e.cred->account, "ioc1");
    }
    testDiag("Reconnect");

    update = pop(sub, evt);
    testEq(update["value"].as<std::string>(), "x509/ioc1");
}

} // namespace

MAIN(testtls)
{
    testPlan(22);
    testSetup();
    logger_config_env();
    testGetSuper();
    testGetIntermediate();
    testGetNameServer();
    testClientReconfig();
     testServerReconfig();
    cleanup_for_valgrind();
    return testDone();
}
