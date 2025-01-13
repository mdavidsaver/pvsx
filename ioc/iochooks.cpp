/*
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 *
 * Author George S. McIntyre <george@level-n.com>, 2023
 *
 */

/**
 * This source file defines the pvxs IOC server instance and also defines a few high level IOC shell commands from PVXS
 * It does not register any database sources defined in any user DB file or user group file, that is
 * handled in records.cpp and groups.cpp respectively.
 */
#include <atomic>
#include <memory>
#include <stdexcept>

#include <epicsExport.h>
#include <epicsExit.h>
#include <epicsString.h>
#include <initHooks.h>
#include <iocsh.h>
#include <dbAccess.h>
#include <dbStaticLib.h>
#include <registryDeviceSupport.h>

#include <pvxs/iochooks.h>
#include <pvxs/log.h>
#include <pvxs/server.h>
#include <pvxs/source.h>

#include "iocshcommand.h"
#include "utilpvt.h"
#include "qsrvpvt.h"

#ifdef USE_QSRV_SINGLE
#  include <dbUnitTest.h>
#endif
#ifdef USE_PVA_LINKS
#  include "pvalink.h"
#endif

// include last to avoid clash of #define printf with other headers
#include <epicsStdio.h>

namespace pvxs {
namespace ioc {

typedef epicsGuard<epicsMutex> Guard;
typedef epicsGuardRelease<epicsMutex> UnGuard;

DEFINE_LOGGER(_logname, "pvxs.ioc");

void printIOCShError(const std::exception& e)
{
    fprintf(stderr, "Error: %s\n", e.what());
}

namespace {
// The pvxs server singleton
struct pvxServer_t {
    epicsMutex lock;
    server::Server srv;
} *pvxServer;

void pvxServerInit() {
    pvxServer = new pvxServer_t();
}
} // namespace

/**
 * Get the plain pvxs server instance
 *
 * @return the pvxs server instance
 */
server::Server server() {
    threadOnce<&pvxServerInit>();
    Guard (pvxServer->lock);
    if(pvxServer->srv)
        return pvxServer->srv;
    else
        throw std::logic_error("No Instance");
}

static
void initialisePvxsServer() {
    using namespace pvxs::server;

    Config conf = ::pvxs::impl::inUnitTest() ? Config::isolated() : Config::from_env();

    threadOnce<&pvxServerInit>();
    Guard G(pvxServer->lock);
    if(!pvxServer->srv) {
        pvxServer->srv = Server(conf);
    }
}

static
void pvxsExitBeforeIocShutdown(void*) noexcept
{
    try {
#ifdef USE_PVA_LINKS
        linkGlobal_t::deinit();
#endif
        Guard (pvxServer->lock);
        if(auto srv = std::move(pvxServer->srv)) {
            assert(!pvxServer->srv);
            srv.stop();
            IOCGroupConfigCleanup();
            log_debug_printf(_logname, "Stopped Server%s", "\n");
        }
    } catch(std::exception& e) {
        fprintf(stderr, "Error in %s : %s\n", __func__, e.what());
    }
}

static
void pvxsExitAfterIocShutdown(void*) noexcept
{
    try {
#ifdef USE_PVA_LINKS
        linkGlobal_t::dtor();
#endif

    } catch(std::exception& e) {
        fprintf(stderr, "Error in %s : %s\n", __func__, e.what());
    }
}

static
void testPrepareImpl()
{
    initialisePvxsServer(); // re-create server for next test cycle
}

void testPrepare()
{
#ifndef USE_PREPARE_CLEANUP_HOOKS
    testPrepareImpl();
#endif
}

void testShutdown()
{
#ifndef USE_DEINIT_HOOKS
    pvxsExitBeforeIocShutdown(nullptr);
#endif
}

void testAfterShutdown()
{
#ifndef USE_DEINIT_HOOKS
    pvxsExitAfterIocShutdown(nullptr);
#endif
}

void testCleanupPrepare()
{
    server::Server trash;
    {
        Guard G(pvxServer->lock);
        trash = std::move(pvxServer->srv);
    }
    resetGroups();
}

////////////////////////////////////
// Two ioc shell commands for pvxs
////////////////////////////////////

/**
 * Show the PVXS server report.
 * The server report is a short list of the EPICS PVA environment variables and
 * a list of registered sources with their IOIDs.
 *
 * @param detail
 */
static
void pvxsr(int detail) {
    if (auto srv = server()) {
        std::ostringstream strm;
        Detailed D(strm, detail);
        strm << srv;
        printf("%s", strm.str().c_str());
    }
}

/**
 * Show information about the PVXS host.
 *
 * Includes:
 *  - OS,
 *  - build toolchain,
 *  - library versions,
 *  - runtime environment information including:
 *    - network address and
 *    - thread count, and
 *  - EPICS PVA environment variable settings
 */
static
void pvxsi() {
    std::ostringstream capture;
    target_information(capture);
    printf("%s", capture.str().c_str());
}

#ifdef USE_QSRV_SINGLE
TestIOC::TestIOC() {
    testdbPrepare();
    testPrepare();
}

void TestIOC::init() {
    if(!isRunning) {
        testIocInitOk();
        isRunning = true;
    }
}

void TestIOC::shutdown() {
    if(isRunning) {
        isRunning = false;
        testShutdown();
        testIocShutdownOk();
        testAfterShutdown();
    }
}

TestIOC::~TestIOC() {
    shutdown();
    testCleanupPrepare();
    testdbCleanup();
}
#endif // USE_QSRV_SINGLE

namespace {

void pvxrefshow() {
    auto refs(instanceSnapshot());
    for(auto& pair : refs) {
        if(pair.second>0u)
            printf("%s\t= %zu\n", pair.first.c_str(), pair.second);
    }
}

struct RefTrack {
    epicsMutex lock;
    std::map<std::string, size_t> refs;
} *refTrack;

void refSavedInit() {
    refTrack = new RefTrack();
}

void pvxrefsave() {
    threadOnce<&refSavedInit>();
    epicsGuard<epicsMutex> G(refTrack->lock);
    refTrack->refs = instanceSnapshot();
}

void pvxrefdiff() {
    auto cur(instanceSnapshot());
    std::map<std::string, int64_t> diff;

    threadOnce<&refSavedInit>();
    {
        epicsGuard<epicsMutex> G(refTrack->lock);

        auto& prev(refTrack->refs);

        // Both std::map will iterate in the same order.  So co-iterate.
        // Some keys may appear in one but not the other.
        auto itC = cur.begin();
        auto itP = prev.begin();
        while(true) {
            bool haveC = itC!=cur.end();
            bool haveP = itP!=prev.end();
            int ord = haveC && haveP ? itC->first.compare(itP->first) : 0;

            if(haveC && haveP && ord==0) {
                diff[itC->first] = int64_t(itC->second) - int64_t(itP->second);
                ++itC;
                ++itP;

            } else if(haveP && (!haveC || ord > 0)) { // !cur || cur > prev
                diff[itP->first] = -int64_t(itP->second);
                ++itP;

            } else if(haveC && (!haveP || ord < 0)) { // !prev || cur < prev
                diff[itC->first] = int64_t(itC->second);
                ++itC;

            } else {
                break;
            }
        }
    }

    for(auto& pair : diff) {
        if(pair.second!=0u)
            printf("%s\t= %lld\n", pair.first.c_str(), (long long)pair.second);
    }
}

void pvxreconfigure()
{
    Guard (pvxServer->lock);
    auto& srv = pvxServer->srv;

    if (srv) {
        printf("Reconfiguring QSRV\n");
        srv.reconfigure(server::Config::from_env());
        pvxsr(0); // print new configuration
    } else {
        fprintf(stderr, "Warning: QSRV not running\n");
    }
}

} // namespace

static
void pvxsInitHook(initHookState theInitHookState) noexcept {
    switch(theInitHookState) {
#ifdef USE_PREPARE_CLEANUP_HOOKS
    case initHookAfterPrepareDatabase: // test only
        testPrepareImpl();
        break;
#endif
    case initHookAtBeginning:
        dbRegisterQSRV2();
        break;
    case initHookAfterCaLinkInit:
#ifdef USE_PVA_LINKS
        linkGlobal_t::alloc();
#endif
#ifndef USE_DEINIT_HOOKS
        // before epicsExit(exitDatabase),
        // so hook registered here will be run after iocShutdown()
    {
        static bool installed = false;
        if(!installed) {
            epicsAtExit(&pvxsExitAfterIocShutdown, nullptr);
            installed = true;
        }
    }
#endif
        break;
    case initHookAfterInitDatabase:
        processGroups();
#ifndef USE_DEINIT_HOOKS
        // register for later cleanup before iocShutdown()
    {
        static bool installed = false;
        if(!installed) {
            epicsAtExit(&pvxsExitBeforeIocShutdown, nullptr);
            installed = true;
        }
    }
#endif
        break;
    case initHookAfterIocBuilt:
#ifdef USE_PVA_LINKS
        linkGlobal_t::init();
#endif
        addSingleSrc();
        addGroupSrc();
        break;
    case initHookAfterIocRunning:
        if(auto srv = server()) {
            srv.start();
            log_debug_printf(_logname, "Started Server%s", "\n");
        }
        break;
    case initHookAfterCaServerPaused:
        if(auto srv = server()) {
            srv.stop();
            log_debug_printf(_logname, "Stopped Server%s", "\n");
        }
        break;
#ifdef USE_DEINIT_HOOKS
    // use de-init hook when available
    case initHookAtShutdown:
        pvxsExitBeforeIocShutdown(nullptr);
        break;
    case initHookAfterShutdown:
        pvxsExitAfterIocShutdown(nullptr);
        break;
#endif
#ifdef USE_PREPARE_CLEANUP_HOOKS
    case initHookBeforeCleanupDatabase: // test only
        testCleanupPrepare();
        break;
#endif
    default:
        break;
    }
}

}
} // namespace pvxs::ioc

using namespace pvxs::ioc;

namespace {

bool enable2() {
    // detect if also linked with qsrv.dbd
    const bool permit = !registryDeviceSupportFind("devWfPDBDemo");
    bool request = permit;
    bool quiet = false;

    auto env_dis = getenv("EPICS_IOC_IGNORE_SERVERS");
    auto env_ena = getenv("PVXS_QSRV_ENABLE");

    if(env_dis && strstr(env_dis, "qsrv2")) {
        request = false;
        quiet = true;

    } else if(env_ena && epicsStrCaseCmp(env_ena, "YES")==0) {
        request = true;

    } else if(env_ena && epicsStrCaseCmp(env_ena, "NO")==0) {
        request = false;
        quiet = true;

    } else if(env_ena) {
        // will be seen during initialization, print synchronously
        fprintf(stderr, "ERROR: PVXS_QSRV_ENABLE=%s not YES/NO.  Defaulting to %s.\n",
                env_ena,
                request ? "YES" : "NO");
    }

    const bool enable = permit && request;

    if(quiet) {
        // shut up, I know what I'm doing...
    } else if(request && !permit) {
        fprintf(stderr,
                "WARNING: QSRV1 detected, disabling QSRV2.\n"
                "         If not intended, omit qsrv.dbd when including pvxsIoc.dbd\n");

    } else {
        printf("INFO: PVXS QSRV2 is loaded, %spermitted, and %s.\n",
               permit ? "" : "NOT ",
               enable ? "ENABLED" : "disabled");

        if(!permit) {
            printf("      Not permitted due to conflict with QSRV1.\n"
                   "      Remove qsrv.dbd from IOC.\n");
        }
    }

    return enable;
}

/**
 * IOC pvxs base registrar.  This implements the required registrar function that is called by xxxx_registerRecordDeviceDriver,
 * the auto-generated stub created for all IOC implementations.
 *
 * It is registered by using the `epicsExportRegistrar()` macro.
 *
 * 1. Specify here all of the commands that you want to be registered and available in the IOC shell.
 * 2. Also make sure that you initialize your server implementation - PVXS in our case - so that it will be available for the shell.
 * 3. Lastly register your hook handler to handle any state hooks that you want to implement
 */
void pvxsBaseRegistrar() noexcept {
    try {
        pvxs::logger_config_env();

        bool enableQ = enable2();

        IOCShCommand<int>("pvxsr", "[show_detailed_information?]", "PVXS Server Report.  "
                                                                   "Shows information about server config (level==0)\n"
                                                                   "or about connected clients (level>0).\n")
                .implementation<&pvxsr>();
        IOCShCommand<>("pvxsi", "Show detailed server information\n").implementation<&pvxsi>();

        IOCShCommand<>("pvxrefshow",
                       "Show instance counts for various internal data structures.\n").implementation<&pvxrefshow>();
        IOCShCommand<>("pvxrefsave",
                       "Save the current set of instance counters for reference by later pvxrefdiff.\n").implementation<&pvxrefsave>();
        IOCShCommand<>("pvxrefdiff",
                       "Show different of current instance counts with those when pvxrefsave was called.\n").implementation<&pvxrefdiff>();
        IOCShCommand<>("pvxreconfigure",
                       "Reconfigure QSRV using current values of EPICS_PVA*.  Only disconnects TLS clients\n")
                .implementation<&pvxreconfigure>();

        // Initialise the PVXS Server
        initialisePvxsServer();

        // Register our hook handler to intercept certain state changes
        initHookRegister(&pvxsInitHook);

        if(enableQ) {
            single_enable();
            group_enable();
            pvalink_enable();
        }
    } catch (std::exception& e) {
        fprintf(stderr, "Error in %s : %s\n", __func__, e.what());
    }
}
} // namespace

extern "C" {
epicsExportRegistrar(pvxsBaseRegistrar);
}
