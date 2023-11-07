/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#ifndef CLIENTIMPL_H
#define CLIENTIMPL_H

#include <list>

#include <epicsTime.h>
#include <epicsEvent.h>
#include <epicsMutex.h>

#include <pvxs/client.h>

#include "evhelper.h"
#include "dataimpl.h"
#include "utilpvt.h"
#include "udp_collector.h"
#include "conn.h"

namespace pvxs {
namespace client {

struct Channel;
struct ContextImpl;

struct ResultWaiter {
    epicsMutex lock;
    epicsEvent notify;
    Result result;
    enum {
        Busy,
        Done,
        Abort,
    } outcome = Busy;

    Value wait(double timeout=-1.0);
    void complete(Result&& result, bool interrupt);
};

// internal actions on an Operation
struct OperationBase : public Operation
{
    const evbase loop;
    // remaining members only accessibly from loop worker
    std::shared_ptr<Channel> chan;
    uint32_t ioid = 0;
    Value result;
    bool done = false;
    std::shared_ptr<ResultWaiter> waiter;

    OperationBase(operation_t op, const evbase& loop);
    virtual ~OperationBase();

    virtual void createOp() =0;
    virtual void disconnected(const std::shared_ptr<OperationBase>& self) =0;

    virtual const std::string& name() override final;
    virtual Value wait(double timeout=-1.0) override final;
    virtual void interrupt() override final;
};

struct RequestFL {
    const size_t limit;
    epicsMutex lock;
    std::vector<Value> unused;

    explicit RequestFL(size_t limit) :limit(limit) {}
};

struct RequestInfo {
    const uint32_t sid, ioid;
    const Operation::operation_t op;
    const std::weak_ptr<OperationBase> handle;

    Value prototype;
    std::shared_ptr<RequestFL> fl;

    RequestInfo(uint32_t sid, uint32_t ioid, std::shared_ptr<OperationBase>& handle);
};

struct Connection final : public ConnBase, public std::enable_shared_from_this<Connection> {
    const std::shared_ptr<ContextImpl> context;
    const bool isTLS;

    // While HoldOff, the time until re-connection
    // While Connected, periodic Echo
    // After early connect() failure, deferred notification
    const evevent echoTimer;

    bool ready = false;
    bool nameserver = false;

    // channels to be created on this Connection in state==Connecting
    std::map<uint32_t, std::weak_ptr<Channel>> pending;

    std::map<uint32_t, std::weak_ptr<Channel>> creatingByCID, // in state==Creating
                                               chanBySID;     // in state==Active

    // entries always have matching entry in a Channel::opByIOID
    std::map<uint32_t, RequestInfo> opByIOID;

    uint32_t nextIOID = 0x10002000u;

    epicsTime connTime;
    std::shared_ptr<const ServerCredentials> cred;

    INST_COUNTER(Connection);

    Connection(const std::shared_ptr<ContextImpl>& context,
               const SockAddr &peerAddr,
               bool reconn, bool isTLS);
    virtual ~Connection();

    static
    std::shared_ptr<Connection> build(const std::shared_ptr<ContextImpl>& context,
                                      const SockAddr& serv,
                                      bool reconn,
                                      bool isTLS);

private:
    void startConnecting();
public:

    void createChannels();

    void sendDestroyRequest(uint32_t sid, uint32_t ioid);

    virtual void bevEvent(short events) override final;

    virtual std::shared_ptr<ConnBase> self_from_this() override final;
    virtual void cleanup() override final;

#define CASE(Op) virtual void handle_##Op() override final;
    CASE(CONNECTION_VALIDATION);
    CASE(CONNECTION_VALIDATED);

    CASE(SEARCH_RESPONSE);
    CASE(CREATE_CHANNEL);
    CASE(DESTROY_CHANNEL);

    CASE(GET);
    CASE(PUT);
    //CASE(PUT_GET);
    CASE(MONITOR);
    CASE(RPC);
    CASE(GET_FIELD);

    CASE(MESSAGE);
#undef CASE

    void handle_GPR(pva_app_msg_t cmd);
protected:
    void tickEcho();
    static void tickEchoS(evutil_socket_t fd, short evt, void *raw);
};

struct ConnectImpl final : public Connect
{
    const evbase loop;
    std::shared_ptr<Channel> chan;
    const std::string _name;
    std::atomic<bool> _connected;
    std::function<void(const Connected&)> _onConn;
    std::function<void()> _onDis;

    ConnectImpl(const evbase& loop, const std::string& name)
        :loop(loop)
        ,_name(name)
        ,_connected{false}
    {}
    virtual ~ConnectImpl();

    virtual const std::string &name() const override final;
    virtual bool connected() const override final;
};

struct Channel {
    const std::shared_ptr<ContextImpl> context;
    const std::string name;
    // Our chosen ID for this channel.
    // used as persistent CID and searchID
    const uint32_t cid;

    enum state_t {
        Searching,  // waiting for a server to claim
        Connecting, // waiting for Connection to become ready
        Creating,   // waiting for reply to CREATE_CHANNEL
        Active,
    } state = Searching;

    bool garbage = false;

    std::shared_ptr<Connection> conn;
    uint32_t sid = 0u;

    // channel created with .server() to bypass normal search process
    SockEndpoint forcedServer;

    // when state==Searching, number of repetitions
    size_t nSearch = 0u;

    // GUID of last positive reply when state!=Searching
    ServerGUID guid{};
    SockAddr replyAddr;

    std::list<std::weak_ptr<OperationBase>> pending;

    // points to storage of Connection::opByIOID
    std::map<uint32_t, RequestInfo*> opByIOID;

    std::list<ConnectImpl*> connectors;

    size_t statTx{}, statRx{};

    INST_COUNTER(Channel);

    Channel(const std::shared_ptr<ContextImpl>& context, const std::string& name, uint32_t cid);
    ~Channel();

    void createOperations();
    void disconnect(const std::shared_ptr<Channel>& self);

    static
    std::shared_ptr<Channel> build(const std::shared_ptr<ContextImpl>& context,
                                   const std::string& name,
                                   const std::string& server);
};

struct Discovery final : public OperationBase
{
    const std::shared_ptr<ContextImpl> context;
    std::function<void(const Discovered &)> notify;
    bool running = false;

    Discovery(const std::shared_ptr<ContextImpl>& context);
    ~Discovery();

    virtual bool cancel() override final;
private:
    bool _cancel(bool implicit);

    // unused for this special case
    virtual void _reExecGet(std::function<void (Result &&)> &&resultcb) override final;
    virtual void _reExecPut(const Value &arg, std::function<void (Result &&)> &&resultcb) override final;
    virtual void createOp() override final;
    virtual void disconnected(const std::shared_ptr<OperationBase> &self) override final;
};

struct ContextImpl : public std::enable_shared_from_this<ContextImpl>
{
    SockAttach attach;
    IfaceMap& ifmap;

    enum state_t {
        Init,
        Running,
        Stopped,
    } state = Init;

    const Config effective;

    const Value caMethod;

    uint32_t nextCID=0x12345678;
    uint32_t prevndrop = 0u;

    evsocket searchTx4, searchTx6;
    uint16_t searchRxPort;

    std::vector<ServerGUID> ignoreServerGUIDs;

    // poked and beaconSenders from both TCP and UDP workers
    epicsMutex pokeLock;
    epicsTimeStamp lastPoke{};
    size_t nPoked = 0u;

    // unlike `poke`, `scheduleInitialSearch` is only ever called from the
    // tcp_loop so this does not need to be guarded by a mutex
    bool initialSearchScheduled = false;

    // map: endpoint+proto -> Beaconer
    typedef std::pair<SockAddr, std::string> BeaconServer;
    struct BeaconInfo {
        SockAddr sender;
        ServerGUID guid{};
        uint8_t peerVersion{};
        epicsTimeStamp time{};
    };
    std::map<BeaconServer, BeaconInfo> beaconTrack;

    std::vector<uint8_t> searchMsg;

    // search destination address and whether to set the unicast flag
    std::vector<std::pair<SockEndpoint, bool>> searchDest;

    size_t currentBucket = 0u;
    // Channels where we have yet to send out an initial search request
    std::list<std::weak_ptr<Channel>> initialSearchBucket;
    // Channels where we are waiting for a search response
    std::vector<std::list<std::weak_ptr<Channel>>> searchBuckets;

    std::list<std::unique_ptr<UDPListener> > beaconRx;

    std::map<uint32_t, std::weak_ptr<Channel>> chanByCID;
    // strong ref. loop through Channel::context
    // explicitly broken by Context::close(), Context::cacheClear(), or ContextImpl::cacheClean()
    // chanByName key'd by (pv, forceServer)
    std::map<std::pair<std::string, std::string>, std::shared_ptr<Channel>> chanByName;

    // pair (addr, useTLS)
    std::map<std::pair<SockAddr, bool>, std::weak_ptr<Connection>> connByAddr;

    std::vector<std::pair<SockEndpoint, std::shared_ptr<Connection>>> nameServers;

    evbase tcp_loop;
    const evevent searchRx4, searchRx6;
    const evevent searchTimer;
    const evevent initialSearcher;

    // beacon handling done on UDP worker.
    // we keep a ref here as long as beaconCleaner is in use
    UDPManager manager;

    std::map<Discovery*, std::weak_ptr<Discovery>> discoverers;

    const evevent beaconCleaner;
    const evevent cacheCleaner;
    const evevent nsChecker;

#ifdef PVXS_ENABLE_OPENSSL
    ossl::SSLContext tls_context;
#endif

    INST_COUNTER(ClientContextImpl);

    ContextImpl(const Config& conf, const evbase &tcp_loop);
    ~ContextImpl();

    void startNS();

    void close();

    void poke();

    void serverEvent(const Discovered &evt);

    void onBeacon(const UDPManager::Beacon& msg);

    void scheduleInitialSearch();

    bool onSearch(evutil_socket_t fd);
    static void onSearchS(evutil_socket_t fd, short evt, void *raw);
    enum class SearchKind { discover, initial, check };
    void tickSearch(SearchKind kind, bool poked);
    static void tickSearchS(evutil_socket_t fd, short evt, void *raw);
    static void initialSearchS(evutil_socket_t fd, short evt, void *raw);
    void tickBeaconClean();
    static void tickBeaconCleanS(evutil_socket_t fd, short evt, void *raw);
    void cacheClean(const std::string &name, Context::cacheAction force);
    static void cacheCleanS(evutil_socket_t fd, short evt, void *raw);
    void onNSCheck();
    static void onNSCheckS(evutil_socket_t fd, short evt, void *raw);
};

struct Context::Pvt {
    // external ref to running loop.
    // impl directly, and indirectly, contains internal refs
private:
    evbase loop;
public:
    const std::shared_ptr<ContextImpl> impl;

    INST_COUNTER(ClientPvt);

    Pvt(const Config& conf);
    ~Pvt(); // I call ContextImpl::close()
};

} // namespace client

} // namespace pvxs

#endif // CLIENTIMPL_H
