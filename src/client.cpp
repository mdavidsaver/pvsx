/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <algorithm>
#include <set>
#include <tuple>

#include <osiSock.h>
#include <dbDefs.h>
#include <epicsThread.h>
#include <epicsGuard.h>

#include <pvxs/log.h>
#include <clientimpl.h>

DEFINE_LOGGER(setup, "pvxs.client.setup");
DEFINE_LOGGER(io, "pvxs.client.io");
DEFINE_LOGGER(beacon, "pvxs.client.beacon");
DEFINE_LOGGER(duppv, "pvxs.client.dup");

typedef epicsGuard<epicsMutex> Guard;
typedef epicsGuardRelease<epicsMutex> UnGuard;

namespace pvxs {
namespace client {

DEFINE_INST_COUNTER(Connection);
DEFINE_INST_COUNTER(Channel);
DEFINE_INST_COUNTER2(ContextImpl, ClientContextImpl);
DEFINE_INST_COUNTER2(Context::Pvt, ClientPvt);

namespace {
/* "normal" tick interval for the search bucket ring, and "fast" interval
 * used for one revolution after a successful poke().
 */
constexpr timeval bucketInterval{1,0};
constexpr timeval bucketIntervalFast{0,200000};
// coalescence time for first search for a batch of newly created Channels
constexpr timeval initialSearchDelay{0, 10000}; // 10 ms
// number of buckets in the search ring
constexpr size_t nBuckets = 30u;

/* our limit for UDP packet payload.
 * try not to fragment with usual MTU==1500 allowing for some overhead
 * by transport protocols.  Ethernet+ip+udp headers add >= 42 bytes.
 * May be more with eg. IP header options, VLAN tag, etc.
 */
constexpr size_t maxSearchPayload = 1400;

/* Interval between checks for Channels which are no longer used by any operation.
 * Channels will be discarded if found to be unused by two consecutive checks.
 */
constexpr timeval channelCacheCleanInterval{10,0};

// time to wait before allowing another hurryUp().
constexpr double pokeHoldoff = 30.0;

// limit on the number of GUIDs * protocols * addresses we will track
constexpr size_t beaconTrackLimit{20000};

// interval between checks to discard servers which have stopped sending beacons
constexpr timeval beaconCleanInterval{180, 0};

// special interval to attempt to reconnect to disconnected name servers
constexpr timeval tcpNSCheckInterval{10, 0};

// searchSequenceID in CMD_SEARCH is redundant.
// So we use a static value and instead rely on IDs for individual PVs
constexpr uint32_t search_seq{0x66696e64}; // "find"
} // namespace

Disconnect::Disconnect()
    :std::runtime_error("Disconnected")
    ,time(epicsTime::getCurrent())
{}

Disconnect::~Disconnect() {}

RemoteError::RemoteError(const std::string& msg)
    :std::runtime_error(msg)
{}

RemoteError::~RemoteError() {}

Finished::~Finished() {}

Connected::Connected(const std::string& peerName,
                     const epicsTime& time,
                     const std::shared_ptr<const pvxs::client::ServerCredentials> &cred)
    :std::runtime_error("Connected")
    ,peerName(peerName)
    ,time(time)
    ,cred(cred)
{}

Connected::~Connected() {}

Interrupted::Interrupted()
    :std::runtime_error ("Interrupted")
{}
Interrupted::~Interrupted() {}

Timeout::Timeout()
    :std::runtime_error ("Timeout")
{}
Timeout::~Timeout() {}

Channel::Channel(const std::shared_ptr<ContextImpl>& context, const std::string& name, uint32_t cid)
    :context(context)
    ,name(name)
    ,cid(cid)
{}

Channel::~Channel()
{
    disconnect(nullptr);
}

void Channel::createOperations()
{
    if(state!=Channel::Active)
        return;

    auto todo = std::move(pending);

    for(auto& wop : todo) {
        auto op = wop.lock();
        if(!op)
            continue;

        uint32_t ioid;
        do {
            ioid = conn->nextIOID++;
        } while(conn->opByIOID.find(ioid)!=conn->opByIOID.end());

        //conn->opByIOID.insert(std::make_pair(ioid, RequestInfo(sid, ioid, op)));
        auto pair = conn->opByIOID.emplace(std::piecewise_construct,
                                           std::forward_as_tuple(ioid),
                                           std::forward_as_tuple(sid, ioid, op));
        opByIOID[ioid] = &pair.first->second;

        op->ioid = ioid;

        op->createOp();
    }
}

// call on disconnect or CMD_DESTROY_CHANNEL
// detach from Connection and notify Connect and *Op
void Channel::disconnect(const std::shared_ptr<Channel>& self)
{
    assert(!self || this==self.get());
    auto current(std::move(conn));

    size_t holdoff = 0u;
    switch(state) {
    case Channel::Connecting:
        current->pending.erase(cid);
        /* disconnect/timeout while before CREATE_CHANNEL sent,
         * likely lower level networking issue.  Try to slow
         * down reconnect loop.
         */
        holdoff = 10u; // arbitrary
        break;
    case Channel::Creating:
        current->creatingByCID.erase(cid);
        break;
    case Channel::Active:
        current->chanBySID.erase(sid);
        break;
    default:
        break;
    }

    if((state==Creating || state==Active) && current && current->connection()) {
        {
            (void)evbuffer_drain(current->txBody.get(), evbuffer_get_length(current->txBody.get()));

            EvOutBuf R(current->sendBE, current->txBody.get());

            to_wire(R, sid);
            to_wire(R, cid);
        }
        statTx += current->enqueueTxBody(CMD_DESTROY_CHANNEL);
    }

    state = Channel::Searching;
    sid = 0xdeadbeef; // spoil

    auto conns(connectors); // copy list

    for(auto& interested : conns) {
        if(interested->_connected.exchange(false, std::memory_order_relaxed) && interested->_onDis)
            interested->_onDis();
    }

    auto ops(std::move(opByIOID));
    for(auto& pair : ops) {
        auto op = pair.second->handle.lock();
        current->opByIOID.erase(pair.first);
        if(op)
            op->disconnected(op);
    }

    if(!self) { // in ~Channel
        // searchBuckets cleaned in tickSearch()

    } else if(forcedServer.addr.family()==AF_UNSPEC) { // begin search

        auto next = (context->currentBucket + holdoff) % nBuckets;

        context->searchBuckets[next].push_back(self);

        log_debug_printf(io, "Server %s detach channel '%s' to re-search\n",
                         current ? current->peerName.c_str() : "<disconnected>",
                         name.c_str());

    } else if(context->state==ContextImpl::Running) { // reconnect to specific server
        conn = Connection::build(context, forcedServer.addr, true,
                                 forcedServer.scheme==SockEndpoint::TLS);

        conn->pending[cid] = self;
        state = Connecting;

        conn->createChannels();

    }
}

Connect::~Connect() {}

ConnectImpl::~ConnectImpl() {}

const std::string& ConnectImpl::name() const
{
    return _name;
}
bool ConnectImpl::connected() const
{
    return _connected.load(std::memory_order_relaxed);
}

std::shared_ptr<Connect> ConnectBuilder::exec()
{
    if(!ctx)
        throw std::logic_error("NULL Builder");

    auto syncCancel(_syncCancel);
    auto context(ctx->impl->shared_from_this());

    auto op(std::make_shared<ConnectImpl>(context->tcp_loop, _pvname));
    op->_onConn = std::move(_onConn);
    op->_onDis = std::move(_onDis);

    std::shared_ptr<ConnectImpl> external(op.get(), [op, syncCancel](ConnectImpl*) mutable {
        // from user thread
        auto temp(std::move(op));
        auto loop(temp->loop);
        // std::bind for lack of c++14 generalized capture
        // to move internal ref to worker for dtor
        loop.tryInvoke(syncCancel, std::bind([](std::shared_ptr<ConnectImpl>& op) {
                      // on worker

                      // ordering of dispatch()/call() ensures creation before destruction
                      assert(op->chan);
                      op->chan->connectors.remove(op.get());
                  }, std::move(temp)));
    });

    auto server(std::move(_server));
    context->tcp_loop.dispatch([op, context, server]() {
        // on worker

        op->chan = Channel::build(context, op->_name, server);

        bool cur = op->_connected = op->chan->state==Channel::Active;
        if(cur && op->_onConn) {
            auto& conn = op->chan->conn;
            Connected evt(conn->peerName, conn->connTime, conn->cred);
            op->_onConn(evt);
        } else if(!cur && op->_onDis) {
            op->_onDis();
        }

        op->chan->connectors.push_back(op.get());
    });

    return external;
}

Value ResultWaiter::wait(double timeout)
{
    Guard G(lock);
    while(outcome==Busy) {
        UnGuard U(G);
        if(!notify.wait(timeout))
            throw Timeout();
    }
    if(outcome==Done)
        return result();
    else
        throw Interrupted();
}

void ResultWaiter::complete(Result&& result, bool interrupt)
{
    {
        Guard G(lock);
        if(outcome!=Busy)
            return;
        this->result = std::move(result);
        outcome = interrupt ? Abort : Done;
    }
    notify.signal();
}

OperationBase::OperationBase(operation_t op, const evbase& loop)
    :Operation(op)
    ,loop(loop)
{}

OperationBase::~OperationBase() {}

const std::string& OperationBase::name()
{
    return chan->name;
}

Value OperationBase::wait(double timeout)
{
    if(!waiter)
        throw std::logic_error("Operation has custom .result() callback");
    return waiter->wait(timeout);
}

void OperationBase::interrupt()
{
    if(waiter)
        waiter->complete(Result(), true);
}

RequestInfo::RequestInfo(uint32_t sid, uint32_t ioid, std::shared_ptr<OperationBase>& handle)
    :sid(sid)
    ,ioid(ioid)
    ,op(handle->op)
    ,handle(handle)
{}

std::shared_ptr<Channel> Channel::build(const std::shared_ptr<ContextImpl>& context,
                                        const std::string& name,
                                        const std::string& server)
{
    if(context->state!=ContextImpl::Running)
        throw std::logic_error("Context close()d");

    SockEndpoint forceServer;
    decltype (context->chanByName)::key_type namekey(name, server);

    if(!server.empty()) {
        SockEndpoint temp(server.c_str(), &context->effective);
        if(!temp.iface.empty() || temp.ttl!=-1)
            throw std::runtime_error(SB()<<"interface or TTL restriction not supported for .server(): "<<server);
        forceServer = std::move(temp);
    }

    std::shared_ptr<Channel> chan;

    auto it = context->chanByName.find(namekey);
    if(it!=context->chanByName.end()) {
        chan = it->second;
        chan->garbage = false;
    }

    if(!chan) {
        while(context->chanByCID.find(context->nextCID)!=context->chanByCID.end())
            context->nextCID++;

        chan = std::make_shared<Channel>(context, name, context->nextCID);

        context->chanByCID[chan->cid] = chan;
        context->chanByName[namekey] = chan;

        if(server.empty()) {
            context->initialSearchBucket.push_back(chan);

            context->scheduleInitialSearch();

        } else { // bypass search and connect so a specific server
            chan->forcedServer = forceServer;
            chan->conn = Connection::build(context, forceServer.addr, false,
                                           forceServer.scheme==SockEndpoint::TLS);

            chan->conn->pending[chan->cid] = chan;
            chan->state = Connecting;

            chan->conn->createChannels();

        }
    }

    return chan;
}

Operation::~Operation() {}

Subscription::~Subscription() {}

Context Context::fromEnv()
{
    return Config::fromEnv().build();
}

Context::Context(const Config& conf)
    :pvt(std::make_shared<Pvt>(conf))
{
    pvt->impl->startNS();
}

Context::~Context() {}

void Context::reconfigure(const Config& newconf)
{
    if(!pvt)
        throw std::logic_error("NULL Context");

#ifdef PVXS_ENABLE_OPENSSL

    ossl::SSLContext new_context;
    if(!newconf.tls_keychain_file.empty()) {
        new_context = ossl::SSLContext::for_client(newconf);
    }

    pvt->impl->manager.loop().call([this, &new_context](){

        log_debug_printf(setup, "Client reconfigure%s", "\n");

        auto conns(std::move(pvt->impl->connByAddr));

        for(auto& pair : conns) {
            auto conn = pair.second.lock();
            conn->cleanup();
        }

        conns.clear();

        pvt->impl->tls_context = new_context;
    });

#else
    pvt->impl->manager.loop().sync();
#endif
}

const Config& Context::config() const
{
    if(!pvt)
        throw std::logic_error("NULL Context");

    return pvt->impl->effective;
}

void Context::close()
{
    if(!pvt)
        throw std::logic_error("NULL Context");

    pvt->impl->close();
}

void Context::hurryUp()
{
    if(!pvt)
        throw std::logic_error("NULL Context");

    pvt->impl->manager.loop().call([this](){
        pvt->impl->poke();
    });
}

void Context::cacheClear(const std::string& name, cacheAction action)
{
    if(!pvt)
        throw std::logic_error("NULL Context");

    pvt->impl->tcp_loop.call([this, name, action](){
        // run twice to ensure both mark and sweep of all unused channels
        log_debug_printf(setup, "cacheClear('%s')\n", name.c_str());
        pvt->impl->cacheClean(name, action);
        pvt->impl->cacheClean(name, action);
    });
}

void Context::ignoreServerGUIDs(const std::vector<ServerGUID>& guids)
{
    if(!pvt)
        throw std::logic_error("NULL Context");

    pvt->impl->manager.loop().call([this, &guids](){
        pvt->impl->ignoreServerGUIDs = guids;
    });
}

Report Context::report(bool zero) const
{
    Report ret;

    pvt->impl->tcp_loop.call([this, &ret, zero](){

        for(auto& pair : pvt->impl->connByAddr) {
            auto conn = pair.second.lock();
            if(!conn)
                continue;

            ret.connections.emplace_back();
            auto& sconn = ret.connections.back();
            sconn.peer = conn->peerName;
            sconn.tx = conn->statTx;
            sconn.rx = conn->statRx;

            if(zero) {
                conn->statTx = conn->statRx = 0u;
            }

            // omit stats for transitory conn->creatingByCID

            for(auto& pair : conn->chanBySID) {
                auto chan = pair.second.lock();
                if(!chan)
                    continue;

                sconn.channels.emplace_back();
                auto& schan = sconn.channels.back();
                schan.name = chan->name;
                schan.tx = chan->statTx;
                schan.rx = chan->statRx;

                if(zero) {
                    chan->statTx = chan->statRx = 0u;
                }
            }
        }

    });

    return ret;
}

static
Value buildCAMethod()
{
    using namespace pvxs::members;

    return TypeDef(TypeCode::Struct, {
                       String("user"),
                       String("host"),
                   }).create();
}

ContextImpl::ContextImpl(const Config& conf, const evbase& tcp_loop)
    :ifmap(IfaceMap::instance())
    ,effective([conf]() -> Config{
        Config eff(conf);
        eff.expand();
        return eff;
    }())
    ,caMethod(buildCAMethod())
    ,searchTx4(AF_INET, SOCK_DGRAM, 0)
    ,searchTx6(AF_INET6, SOCK_DGRAM, 0)
    ,tcp_loop(tcp_loop)
    ,searchRx4(__FILE__, __LINE__,
               event_new(tcp_loop.base, searchTx4.sock, EV_READ|EV_PERSIST, &ContextImpl::onSearchS, this))
    ,searchRx6(__FILE__, __LINE__,
               event_new(tcp_loop.base, searchTx6.sock, EV_READ|EV_PERSIST, &ContextImpl::onSearchS, this))
    ,searchTimer(__FILE__, __LINE__,
                 event_new(tcp_loop.base, -1, EV_TIMEOUT, &ContextImpl::tickSearchS, this))
    ,initialSearcher(__FILE__, __LINE__,
                     event_new(tcp_loop.base, -1, EV_TIMEOUT, &ContextImpl::initialSearchS, this))
    ,manager(UDPManager::instance(effective.shareUDP()))
    ,beaconCleaner(__FILE__, __LINE__,
                   event_new(manager.loop().base, -1, EV_TIMEOUT|EV_PERSIST, &ContextImpl::tickBeaconCleanS, this))
    ,cacheCleaner(__FILE__, __LINE__,
                  event_new(tcp_loop.base, -1, EV_TIMEOUT|EV_PERSIST, &ContextImpl::cacheCleanS, this))
    ,nsChecker(__FILE__, __LINE__,
               event_new(tcp_loop.base, -1, EV_TIMEOUT|EV_PERSIST, &ContextImpl::onNSCheckS, this))
{
#ifdef PVXS_ENABLE_OPENSSL
    if(!effective.tls_keychain_file.empty()) {
        try {
            tls_context = ossl::SSLContext::for_client(effective);
        }catch(std::exception& e){
            log_err_printf(setup, "Unable to setup TLS.  Disabled for client : %s\n", e.what());
        }
    }
#endif

    searchBuckets.resize(nBuckets);

    std::set<SockAddr, SockAddrOnlyLess> bcasts;
    for(auto& addr : searchTx4.broadcasts()) {
        addr.setPort(0u);
        bcasts.insert(addr);
    }

    searchTx6.ipv6_only();

    {
        auto any(SockAddr::any(searchTx4.af));
        if(bind(searchTx4.sock, &any->sa, any.size()))
            throw std::runtime_error("Unable to bind random UDP port");

        socklen_t alen = any.capacity();
        if(getsockname(searchTx4.sock, &any->sa, &alen))
            throw std::runtime_error("Unable to readback random UDP port");

        searchRxPort = any.port();

        log_debug_printf(setup, "Using UDP Rx port %u\n", searchRxPort);
    }
    {
        auto any(SockAddr::any(searchTx6.af, searchRxPort));
        if(bind(searchTx6.sock, &any->sa, any.size()))
            throw std::runtime_error("Unable to bind random UDP6 port");
    }

    searchTx4.set_broadcast(true);
    searchTx4.enable_SO_RXQ_OVFL();
    searchTx6.enable_SO_RXQ_OVFL();

    for(auto& addr : effective.addressList) {
        SockEndpoint ep;
        try {
            ep = SockEndpoint(addr, nullptr, effective.udp_port);
        }catch(std::exception& e){
            log_warn_printf(setup, "%s  Ignoring malformed address %s\n", e.what(), addr.c_str());
            continue;
        }
        assert(ep.addr.family()==AF_INET || ep.addr.family()==AF_INET6);

        // if !bcast and !mcast
        auto isucast = !ep.addr.isMCast();

        if(isucast && ep.addr.family()==AF_INET && bcasts.find(ep.addr)!=bcasts.end())
            isucast = false;

        log_info_printf(io, "Searching to %s%s\n", std::string(SB()<<ep).c_str(), (isucast?" unicast":""));
        searchDest.emplace_back(ep, isucast);
    }

    for(auto& addr : effective.nameServers) {
        SockEndpoint saddr;
        try {
            SockEndpoint temp(addr.c_str(), &effective);
            if(!temp.iface.empty() || temp.ttl!=-1)
                throw std::runtime_error(SB()<<"interface or TTL restriction not supported for nameserver: "<<addr);
            saddr = std::move(temp);
        }catch(std::runtime_error& e) {
            log_err_printf(setup, "%s  Ignoring...\n", e.what());
        }

        log_info_printf(io, "Searching to TCP %s\n", std::string(SB()<<saddr).c_str());
        nameServers.emplace_back(saddr, nullptr);
    }

    const auto cb([this](const UDPManager::Beacon& msg) {
        onBeacon(msg);
    });

    for(auto& iface : effective.interfaces) {
        SockEndpoint addr(iface.c_str(), nullptr, effective.udp_port);
        beaconRx.push_back(manager.onBeacon(addr, cb));
        log_info_printf(io, "Listening for beacons on %s\n", addr.addr.tostring().c_str());

        if(addr.addr.family()==AF_INET && addr.addr.isAny()) {
            // if listening on 0.0.0.0, also listen on [::]
            auto any6(addr);
            any6.addr = SockAddr::any(AF_INET6);

            beaconRx.push_back(manager.onBeacon(any6, cb));
        }
    }

    for(auto& listener : beaconRx) {
        listener->start();
    }

    if(event_add(searchTimer.get(), &bucketInterval))
        log_err_printf(setup, "Error enabling search timer\n%s", "");
    if(event_add(searchRx4.get(), nullptr))
        log_err_printf(setup, "Error enabling search RX4\n%s", "");
    if(event_add(searchRx6.get(), nullptr))
        log_err_printf(setup, "Error enabling search RX6\n%s", "");
    if(event_add(beaconCleaner.get(), &beaconCleanInterval))
        log_err_printf(setup, "Error enabling beacon clean timer on\n%s", "");
    if(event_add(cacheCleaner.get(), &channelCacheCleanInterval))
        log_err_printf(setup, "Error enabling channel cache clean timer on\n%s", "");

    state = Running;
}

ContextImpl::~ContextImpl() {}

void ContextImpl::startNS()
{
    if(nameServers.empty()) // vector size const after ctor, contents remain mutable
        return;

    tcp_loop.call([this]() {
        // start connections to name servers
        for(auto& ns : nameServers) {
            const auto& serv = ns.first;
            ns.second = Connection::build(shared_from_this(), serv.addr, false,
                                          serv.scheme==SockEndpoint::TLS);
            ns.second->nameserver = true;
            log_debug_printf(io, "Connecting to nameserver %s%s\n",
                             ns.second->peerName.c_str(), ns.second->isTLS ? " TLS" : "");
        }

        if(event_add(nsChecker.get(), &tcpNSCheckInterval))
            log_err_printf(setup, "Error enabling TCP search reconnect timer\n%s", "");
    });
}

void ContextImpl::close()
{
    log_debug_printf(setup, "context %p close\n", this);

    // terminate all active connections
    tcp_loop.call([this]() {
        if(state == Stopped)
            return;
        state = Stopped;

        (void)event_del(searchTimer.get());
        (void)event_del(searchRx4.get());
        (void)event_del(searchRx6.get());
        (void)event_del(beaconCleaner.get());
        (void)event_del(cacheCleaner.get());

        auto conns(std::move(connByAddr));
        // explicitly break ref. loop of channel cache
        auto chans(std::move(chanByName));

        for(auto& pair : conns) {
            auto conn = pair.second.lock();
            if(!conn)
                continue;

            conn->cleanup();
        }

        conns.clear();
        chans.clear();
        // breaks a ref. loop between Connection and ClientContextImpl
        nameServers.clear();

        // internal_self.use_count() may be >1 if
        // we are orphaning some Operations
    });

    tcp_loop.sync();

    // ensure any in-progress callbacks have completed
    manager.sync();
}

void ContextImpl::poke()
{
    {
        Guard G(pokeLock);
        if(nPoked)
            return;

        epicsTimeStamp now{};

        double age = -1.0;
        if(epicsTimeGetCurrent(&now) || (age=epicsTimeDiffInSeconds(&now, &lastPoke))<pokeHoldoff) {
            log_debug_printf(setup, "Ignoring hurryUp() age=%.1f sec\n", age);
            return;
        }
        lastPoke = now;
        nPoked = nBuckets;
    }

    log_debug_printf(setup, "hurryUp()%s\n", "");

    timeval immediate{0,0};
    if(event_add(searchTimer.get(), &immediate))
        throw std::runtime_error("Unable to schedule searchTimer");
}

void ContextImpl::scheduleInitialSearch()
{
    if (!initialSearchScheduled)
    {
        log_debug_printf(setup, "%s()\n", __func__);

        initialSearchScheduled = true;
        if (event_add(initialSearcher.get(), &initialSearchDelay))
            throw std::runtime_error("Unable to schedule initialSearcher");
    }
}

void ContextImpl::onBeacon(const UDPManager::Beacon& msg)
{
    epicsTimeStamp now;
    epicsTimeGetCurrent(&now);

    Guard G(pokeLock);

    const decltype (beaconTrack)::key_type key(msg.server, msg.proto);

    auto it = beaconTrack.find(key);

    enum {
        Update,
        Change,
        New,
    } action = Update;

    if(it==beaconTrack.end()) {
        if(beaconTrack.size() >= beaconTrackLimit) {
            // Overloaded.  Assume that some server is in a fast restart loop.
            // Ignore it, and continue tracking other/older servers.
            log_debug_printf(beacon, "Tracking too many beacons, ignoring %s\n",
                             std::string(SB()<<msg.src<<" "<<msg.guid<<' '<<msg.server).c_str());
            return;
        }
        auto pair(beaconTrack.emplace(key, BeaconInfo()));
        assert(pair.second); // we just checked that this key is not there.
        it = pair.first;

        action = New;
    }

    auto& cur(it->second);

    if(action==Update && (cur.guid!=msg.guid || cur.peerVersion!=msg.peerVersion)) {
        action = Change;
        log_debug_printf(beacon, "Update server %s\n",
                         std::string(SB()<<msg.src<<" : "<<msg.server<<'/'<<msg.proto
                                     <<" "<<cur.guid<<'/'<<(unsigned)cur.peerVersion
                                     <<" -> "<<msg.guid<<'/'<<(unsigned)msg.peerVersion).c_str());

        serverEvent(Discovered{Discovered::Timeout,
                               cur.peerVersion,
                               msg.src.tostring(),
                               it->first.second,
                               it->first.first.tostring(),
                               cur.guid,
                               now
                    });
    }

    cur.guid = msg.guid;
    cur.peerVersion = msg.peerVersion;
    cur.time = now;
    // don't trigger if sender changes as server configuration
    // could see beacons reach us from multiple interfaces.
    cur.sender = msg.src;

    if(action!=Update) {
        if(action==New)
            log_debug_printf(beacon, "New server %s\n",
                             std::string(SB()<<msg.src<<" : "<<msg.server<<'/'<<msg.proto
                                         <<" "<<cur.guid<<'/'<<(unsigned)cur.peerVersion).c_str());

        serverEvent(Discovered{Discovered::Online,
                               msg.peerVersion,
                               msg.src.tostring(),
                               msg.proto,
                               msg.server.tostring(),
                               msg.guid,
                               now
                    });

        poke();
    }
}

static
void procSearchReply(ContextImpl& self, const SockAddr& src, uint8_t peerVersion, Buffer& M, bool istcp)
{
    ServerGUID guid;
    SockAddr serv;
    uint16_t port = 0;
    uint8_t found = 0u;
    uint32_t seq = 0u;

    _from_wire<12>(M, &guid[0], false, __FILE__, __LINE__);
    // searchSequenceID
    // we don't use this for normal search and instead rely on ID for individual PVs
    from_wire(M, seq);

    from_wire(M, serv);
    if(serv.isAny())
        serv = src;
    from_wire(M, port);
    if(istcp && port==0)
        port = src.port();
    serv.setPort(port);

    std::string proto;
    from_wire(M, proto);
    from_wire(M, found);

    uint16_t nSearch = 0u;
    from_wire(M, nSearch);

    if(M.good()) {
        for(const ServerGUID& ignore : self.ignoreServerGUIDs) {
            if(guid==ignore) {
                log_info_printf(io, "Ignore reply from %s with %s\n",
                                 src.tostring().c_str(), std::string(SB()<<guid).c_str());
                return;
            }
        }
    }

    if(M.good() && !istcp && seq==search_seq && nSearch==0u && !found && !self.discoverers.empty()) {
        // a discovery pong, process this like a beacon
        log_debug_printf(io, "Discover reply for %s\n", src.tostring().c_str());

        UDPManager::Beacon fakebeacon{src};
        fakebeacon.proto = proto;
        fakebeacon.server = serv;
        fakebeacon.guid = guid;
        fakebeacon.peerVersion = peerVersion;

        self.onBeacon(fakebeacon);
    }

    bool isTCP = proto=="tcp";

#ifdef PVXS_ENABLE_OPENSSL
    bool isTLS = proto=="tls";
    if(!self.tls_context && isTLS)
        return;
#else
    const bool isTLS = false;
#endif
    if(!found || !(isTCP || isTLS))
        return;

    for(auto n : range(nSearch)) {
        (void)n;

        uint32_t id=0u;
        from_wire(M, id);
        if(!M.good())
            break;

        std::shared_ptr<Channel> chan;
        {
            auto it = self.chanByCID.find(id);
            if(it==self.chanByCID.end())
                continue;

            chan = it->second.lock();
            if(!chan)
                continue;
        }

        log_debug_printf(io, "Search reply for %s\n", chan->name.c_str());

        if(chan->state==Channel::Searching) {
            chan->guid = guid;
            chan->replyAddr = serv;

            chan->conn = Connection::build(self.shared_from_this(), serv, false, isTLS);

            chan->conn->pending[chan->cid] = chan;
            chan->nSearch = 0u;
            chan->state = Channel::Connecting;

            chan->conn->createChannels();

        } else if(chan->guid!=guid) {
            log_err_printf(duppv, "Duplicate PV name %s from %s and %s\n",
                           chan->name.c_str(),
                           chan->replyAddr.tostring().c_str(),
                           serv.tostring().c_str());
        }
    }

}

bool ContextImpl::onSearch(evutil_socket_t fd)
{
    searchMsg.resize(0x10000);
    SockAddr src;

    recvfromx rx{fd, (char*)&searchMsg[0], searchMsg.size()-1, &src};
    const int nrx = rx.call();

    if(nrx>=0 && rx.ndrop!=0 && prevndrop!=rx.ndrop) {
        log_debug_printf(io, "UDP search reply buffer overflow %u -> %u\n", unsigned(prevndrop), unsigned(rx.ndrop));
        prevndrop = rx.ndrop;
    }

    if(nrx<0) {
        int err = evutil_socket_geterror(fd);
        if(err==SOCK_EWOULDBLOCK || err==EAGAIN || err==SOCK_EINTR) {
            // nothing to do here
        } else {
            log_warn_printf(io, "UDP search RX Error on : %s\n",
                       evutil_socket_error_to_string(err));
        }
        return false; // wait for more I/O

    }

    FixedBuf M(true, searchMsg.data(), nrx);
    Header head{};
    from_wire(M, head); // overwrites M.be

    if(!M.good() || (head.flags&(pva_flags::Control|pva_flags::SegMask))) {
        // UDP packets can't contain control messages, or use segmentation

        log_hex_printf(io, Level::Debug, &searchMsg[0], nrx, "Ignore UDP message from %s\n", src.tostring().c_str());
        return true;
    }

    log_hex_printf(io, Level::Debug, &searchMsg[0], nrx, "UDP search Rx %d from %s\n", nrx, src.tostring().c_str());

    if(head.len > M.size() && M.good()) {
        log_info_printf(io, "UDP ignore header truncated%s", "\n");
        return true;
    }

    if(head.cmd==CMD_SEARCH_RESPONSE) {
        procSearchReply(*this, src, head.version, M, false);

    } else {
        M.fault(__FILE__, __LINE__);
    }

    if(!M.good()) {
        log_hex_printf(io, Level::Err, &searchMsg[0], nrx,
                "%s:%d Invalid search reply %d from %s\n",
                M.file(), M.line(), nrx, src.tostring().c_str());
    }

    return true;
}

void Connection::handle_SEARCH_RESPONSE()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    procSearchReply(*context, peerAddr, peerVersion, M, true);

    if(!M.good()) {
        log_crit_printf(io, "%s:%d Server %s sends invalid SEARCH_RESPONSE.  Disconnecting...\n",
                        M.file(), M.line(), peerName.c_str());
        bev.reset();
    }
}

void ContextImpl::onSearchS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        log_debug_printf(io, "UDP search Rx event %x\n", evt);
        if(!(evt&EV_READ))
            return;

        // limit number of packets processed before going back to the reactor
        unsigned i;
        const unsigned limit = 40;
        for(i=0; i<limit && static_cast<ContextImpl*>(raw)->onSearch(fd); i++) {}
        log_debug_printf(io, "UDP search processed %u/%u\n", i, limit);

    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in search Rx callback: %s\n", e.what());
    }
}

void ContextImpl::tickSearch(SearchKind kind, bool poked)
{
    // If kind == SearchKind::discover, then this is a discovery ping.
    // these are really empty searches with must-reply set.
    // So if !discover, then we should not be modifying any internal state
    //
    // If kind == SearchKind::initial we are sending the first search request
    // for the channels in initalSearchBucket, and not resending requests for
    // channels in the searchBuckets.

    auto idx = currentBucket;
    if(kind == SearchKind::check)
        currentBucket = (currentBucket+1u)%searchBuckets.size();

    log_debug_printf(io, "Search tick %zu\n", idx);

    decltype (searchBuckets)::value_type bucket;
    if (kind == SearchKind::initial) {
        initialSearchBucket.swap(bucket);
    } else if(kind == SearchKind::check) {
        searchBuckets[idx].swap(bucket);
    }

    while(!bucket.empty() || kind == SearchKind::discover) {
        // when 'discover' we only loop once

        searchMsg.resize(0x10000);
        FixedBuf M(true, searchMsg.data(), searchMsg.size());
        M.skip(8, __FILE__, __LINE__); // fill in header after body length known

        // searchSequenceID
        to_wire(M, search_seq);

        // flags and reserved.
        // initially flags[7] is cleared (bcast)
        auto pflags = M.save();
        to_wire(M, uint8_t(kind == SearchKind::discover ?
                           pva_search_flags::MustReply : 0u)); // must-reply to discovery, ignore regular negative search
        to_wire(M, uint8_t(0u));
        to_wire(M, uint16_t(0u));

        // IN6ADDR_ANY_INIT
        to_wire(M, uint32_t(0u));
        to_wire(M, uint32_t(0u));
        to_wire(M, uint32_t(0u));
        to_wire(M, uint32_t(0u));

        auto pport = M.save();
        to_wire(M, uint16_t(searchRxPort));

        if(kind == SearchKind::discover) {
            to_wire(M, uint8_t(0u));

#ifdef PVXS_ENABLE_OPENSSL
        } else if(tls_context) {
            to_wire(M, uint8_t(2u));
            to_wire(M, "tls");
            to_wire(M, "tcp");
#endif

        } else {
            to_wire(M, uint8_t(1u));
            to_wire(M, "tcp");
        }

        // placeholder for channel count;
        auto pcount = M.save();
        uint16_t count = 0u;
        M.skip(2u, __FILE__, __LINE__);

        bool payload = false;
        while(!bucket.empty()) {
            assert(kind != SearchKind::discover);

            auto chan = bucket.front().lock();
            if(!chan || chan->state!=Channel::Searching) {
                bucket.pop_front();
                continue;
            }

            auto save = M.save();
            to_wire(M, uint32_t(chan->cid));
            to_wire(M, chan->name);

            if(!M.good()) {
                // some absurdly long PV name?
                log_err_printf(io, "PV name exceeds search buffer: '%s'\n", chan->name.c_str());
                // drop it on the floor
                bucket.pop_front();
                continue;

            } else if(size_t(M.save() - searchMsg.data()) > maxSearchPayload) {
                if(payload) {
                    // other names did fit, defer this one to the next packet
                    M.restore(save);
                    break;

                } else {
                    // some slightly less absurdly long PV name.
                    // Less than the UDP packet limit, but longer
                    // than typical MTU.  Try to send, probably
                    // no choice but to fragment.
                }
            }

            count++;

            size_t ninc = 0u;
            if(kind==SearchKind::check && !poked)
                ninc = chan->nSearch = std::min(searchBuckets.size(), chan->nSearch+1u);
            auto next = (idx + ninc)%searchBuckets.size();
            auto nextnext = (next + 1u)%searchBuckets.size();

            // try to smooth out UDP bcast load by waiting one extra tick
            {
                auto nextN = searchBuckets[next].size();
                auto nextnextN = searchBuckets[nextnext].size();

                if(nextN > nextnextN && (nextN-nextnextN > 100u))
                    next = nextnext;
            }

            auto& nextBucket = searchBuckets[next];

            nextBucket.splice(nextBucket.end(),
                              bucket,
                              bucket.begin());
            payload = true;
        }
        assert(M.good());

        if(!payload && kind != SearchKind::discover)
            break;

        {
            FixedBuf C(true, pcount, 2u);
            to_wire(C, count);
        }
        size_t consumed = M.save() - searchMsg.data();
        {
            FixedBuf H(true, searchMsg.data(), 8);
            to_wire(H, Header{CMD_SEARCH, 0, uint32_t(consumed-8u)});
        }
        for(auto& pair : searchDest) {
            auto& dest = pair.first.addr.family()==AF_INET ? searchTx4 : searchTx6;

            if(pair.second) {
                *pflags |= pva_search_flags::Unicast;

            } else {
                *pflags &= ~pva_search_flags::Unicast;

                dest.mcast_prep_sendto(pair.first);
            }

            int ntx = sendto(dest.sock, (char*)searchMsg.data(), consumed, 0,
                             &pair.first.addr->sa, pair.first.addr.size());

            if(ntx<0) {
                int err = evutil_socket_geterror(dest.sock);
                auto lvl = Level::Warn;
                if(err==EINTR || err==EPERM)
                    lvl = Level::Debug;
                log_printf(io, lvl, "Search tx %s error (%d) %s\n",
                           pair.first.addr.tostring().c_str(), err, evutil_socket_error_to_string(err));

            } else if(unsigned(ntx)<consumed) {
                log_warn_printf(io, "Search truncated %u < %u",
                           unsigned(ntx), unsigned(consumed));

            } else {
                log_hex_printf(io, Level::Debug, (char*)searchMsg.data(), consumed,
                               "Search to %s %s\n",
                               std::string(SB()<<pair.first).c_str(),
                               pair.second ? "ucast" : "bcast");
            }
        }
        *pflags |= 0x80; // TCP search is always "unicast"
        // TCP search replies should always come back on the same connection,
        // so zero out the meaningless response port.
        pport[0] = pport[1] = 0;

        for(auto& pair : nameServers) {
            auto& serv = pair.second;

            if(!serv->ready || !serv->connection())
                continue;

            auto tx = bufferevent_get_output(serv->connection());

            // arbitrarily skip searching if TX buffer is too full
            // TODO: configure limit?
            if(evbuffer_get_length(tx) > 64*1024u)
                continue;

            (void)evbuffer_add(tx, (char*)searchMsg.data(), consumed);
            // fail silently, will retry
        }

        if(kind == SearchKind::discover)
            break;
    }
}

void ContextImpl::tickSearchS(evutil_socket_t fd, short evt, void *raw)
{
    auto self(static_cast<ContextImpl*>(raw));
    try {
        bool poke = false;
        {
            Guard G(self->pokeLock);
            if(self->nPoked) {
                poke = true;
                self->nPoked--;
            }
        }

        self->tickSearch(SearchKind::check, poke);

        if(event_add(self->searchTimer.get(), poke ? &bucketIntervalFast : &bucketInterval))
            log_err_printf(setup, "Error re-enabling search timer on\n%s", "");

    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in search timer callback: %s\n", e.what());
    }
}

void ContextImpl::initialSearchS(evutil_socket_t fd, short evt, void *raw)
{
    auto self(static_cast<ContextImpl*>(raw));
    try {
        self->initialSearchScheduled = false;
        self->tickSearch(SearchKind::initial, false);
    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in initial search callback: %s\n", e.what());
    }
}

void ContextImpl::tickBeaconClean()
{
    epicsTimeStamp now;
    epicsTimeGetCurrent(&now);

    Guard G(pokeLock);

    auto it = beaconTrack.begin();
    while(it!=beaconTrack.end()) {
        auto cur = it++;

        double age = epicsTimeDiffInSeconds(&now, &cur->second.time);

        if(age < -15.0 || age > 2*beaconCleanInterval.tv_sec) {
            log_debug_printf(io, "%s\n",
                             std::string(SB()<<" Lost server "<<cur->second.guid
                                         <<' '<<cur->first.second<<'/'<<cur->first.first).c_str());

            serverEvent(Discovered{Discovered::Timeout,
                                   cur->second.peerVersion,
                                   "", // no associated Beacon
                                   cur->first.second,
                                   cur->first.first.tostring(),
                                   cur->second.guid,
                                   now
                        });

            beaconTrack.erase(cur);
        }
    }
}

void ContextImpl::tickBeaconCleanS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        static_cast<ContextImpl*>(raw)->tickBeaconClean();
    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in beacon cleaner timer callback: %s\n", e.what());
    }
}

void ContextImpl::onNSCheck()
{
    for(auto& ns : nameServers) {
        if(ns.second && ns.second->state != ConnBase::Disconnected) // hold-off, connecting, or connected
            continue;

        ns.second = Connection::build(shared_from_this(), ns.first.addr, false,
                                      ns.first.scheme==SockEndpoint::TLS);
        ns.second->nameserver = true;
        log_debug_printf(io, "Reconnecting nameserver %s\n", ns.second->peerName.c_str());
    }
}

void ContextImpl::onNSCheckS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        static_cast<ContextImpl*>(raw)->onNSCheck();
    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in TCP nameserver timer callback: %s\n", e.what());
    }
}

void ContextImpl::cacheClean(const std::string& name, Context::cacheAction action)
{
    auto next(chanByName.begin()),
         end(chanByName.end());

    while(next!=end) {
        auto cur(next++);

        if(!name.empty() && cur->first.first!=name)
            continue;

        else if(action!=Context::Clean || cur->second.use_count()<=1) {
            cur->second->garbage = true;

            if(action==Context::Clean && !cur->second->garbage) {
                // mark for next sweep
                log_debug_printf(setup, "Chan GC mark '%s':'%s'\n",
                                 cur->first.first.c_str(), cur->first.second.c_str());

            } else {
                log_debug_printf(setup, "Chan GC sweep '%s':'%s'\n",
                                 cur->first.first.c_str(), cur->first.second.c_str());

                auto trash(std::move(cur->second));

                // explicitly break ref. loop of channel cache
                chanByName.erase(cur);

                if(action==Context::Disconnect) {
                    trash->disconnect(trash);
                }
            }
        }
    }
}

void ContextImpl::cacheCleanS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        static_cast<ContextImpl*>(raw)->cacheClean(std::string(), Context::Clean);
        static_cast<ContextImpl*>(raw)->tickBeaconClean();
    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in beacon cleaner timer callback: %s\n", e.what());
    }
}

Context::Pvt::Pvt(const Config& conf)
    :loop("PVXCTCP", epicsThreadPriorityCAServerLow)
    ,impl(std::make_shared<ContextImpl>(conf, loop.internal()))
{}

Context::Pvt::~Pvt()
{
    impl->close();
}

} // namespace client

} // namespace pvxs
