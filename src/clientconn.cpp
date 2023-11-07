/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <osiProcess.h>

#include <pvxs/log.h>
#include "clientimpl.h"

namespace pvxs {
namespace client {

DEFINE_LOGGER(io, "pvxs.client.io");
DEFINE_LOGGER(connsetup, "pvxs.tcp.setup");
DEFINE_LOGGER(remote, "pvxs.remote.log");

Connection::Connection(const std::shared_ptr<ContextImpl>& context,
                       const SockAddr& peerAddr,
                       bool reconn,
                       bool isTLS)
    :ConnBase (true, context->effective.sendBE(),
               nullptr,
               peerAddr)
    ,context(context)
    ,isTLS(isTLS)
    ,echoTimer(__FILE__, __LINE__,
               event_new(context->tcp_loop.base, -1, EV_TIMEOUT|EV_PERSIST, &tickEchoS, this))
{
    if(reconn) {
        log_debug_printf(io, "start holdoff timer for %s\n", peerName.c_str());

        constexpr timeval holdoff{2, 0};
        if(event_add(echoTimer.get(), &holdoff))
            log_err_printf(io, "Server %s error starting echoTimer as holdoff\n", peerName.c_str());

    } else {
        startConnecting();
    }
}

Connection::~Connection()
{
    log_debug_printf(io, "Cleaning connection to %s\n", peerName.c_str());
    cleanup();
}

std::shared_ptr<Connection> Connection::build(const std::shared_ptr<ContextImpl>& context,
                                              const SockAddr& serv, bool reconn, bool tls)
{
    if(context->state!=ContextImpl::Running)
        throw std::logic_error("Context close()d");

    auto pair(std::make_pair(serv, tls));
    std::shared_ptr<Connection> ret;
    auto it = context->connByAddr.find(pair);
    if(it==context->connByAddr.end() || !(ret = it->second.lock())) {
        context->connByAddr[pair] = ret = std::make_shared<Connection>(context, serv, reconn, tls);
    }
    return ret;
}

void Connection::startConnecting()
{
    assert(!this->bev);

    decltype(this->bev) bev(__FILE__, __LINE__,
                bufferevent_socket_new(context->tcp_loop.base, -1,
                                       BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS));

#ifdef PVXS_ENABLE_OPENSSL
    if(isTLS) {
        auto ctx(SSL_new(context->tls_context.ctx));
        if(!ctx)
            throw ossl::SSLError("SSL_new");

        // w/ BEV_OPT_CLOSE_ON_FREE calls SSL_free() on error
        bev.reset(bufferevent_openssl_socket_new(context->tcp_loop.base,
                                                 -1,
                                                 ctx,
                                                 BUFFEREVENT_SSL_CONNECTING,
                                                 BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS));

        // added with libevent 2.2.1-alpha
        //(void)bufferevent_ssl_set_flags(bev.get(), BUFFEREVENT_SSL_DIRTY_SHUTDOWN);
        // deprecated, but not yet removed
        bufferevent_openssl_set_allow_dirty_shutdown(bev.get(), 1);

    } else
#endif
    {
        bev.reset(bufferevent_socket_new(context->tcp_loop.base,
                                         -1,
                                         BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS));
    }

    bufferevent_setcb(bev.get(), &bevReadS, nullptr, &bevEventS, this);

    timeval tmo(totv(context->effective.tcpTimeout));
    bufferevent_set_timeouts(bev.get(), &tmo, &tmo);

    if(bufferevent_socket_connect(bev.get(), const_cast<sockaddr*>(&peerAddr->sa), peerAddr.size())) {
        // non-blocking connect() failed immediately.
        // try to defer notification.
        state = Disconnected;
        constexpr timeval immediate{0, 0};
        if(event_add(echoTimer.get(), &immediate))
            throw std::runtime_error(SB()<<"Unable to begin connecting or schedule deferred notification "<<peerName);
        log_warn_printf(io, "Unable to connect() to %s\n", peerName.c_str());
        return;
    }

    connect(std::move(bev));

    log_debug_printf(io, "Connecting to %s, RX readahead %zu%s\n",
                     peerName.c_str(), readahead, isTLS ? " TLS" : "");
}

void Connection::createChannels()
{
    if(!ready)
        return; // defer until CONNECTION_VALIDATED

    (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

    auto todo = std::move(pending);

    for(auto& pair : todo) {
        auto chan = pair.second.lock();
        if(!chan || chan->state!=Channel::Connecting)
            continue;

        {
            (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

            EvOutBuf R(sendBE, txBody.get());

            to_wire(R, uint16_t(1u));
            to_wire(R, chan->cid);
            to_wire(R, chan->name);
        }
        chan->statTx += enqueueTxBody(CMD_CREATE_CHANNEL);

        creatingByCID[chan->cid] = chan;
        chan->state = Channel::Creating;

        log_debug_printf(io, "Server %s creating channel '%s' (%u)\n", peerName.c_str(),
                         chan->name.c_str(), unsigned(chan->cid));
    }
}

void Connection::sendDestroyRequest(uint32_t sid, uint32_t ioid)
{
    if(!bev)
        return;
    {
        (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

        EvOutBuf R(sendBE, txBody.get());

        to_wire(R, sid);
        to_wire(R, ioid);
    }
    enqueueTxBody(CMD_DESTROY_REQUEST);

}

void Connection::bevEvent(short events)
{
#ifdef PVXS_ENABLE_OPENSSL
    if((events & (BEV_EVENT_ERROR|BEV_EVENT_EOF)) && isTLS && bev) {
        while(auto err = bufferevent_get_openssl_error(bev.get())) {
            log_err_printf(io, "TLS Error (0x%lx) %s\n",
                           err, ERR_reason_error_string(err));
        }
    }
#endif
    ConnBase::bevEvent(events);
    // called Connection::cleanup()

    if(bev && (events&BEV_EVENT_CONNECTED)) {
        log_debug_printf(io, "Connected to %s\n", peerName.c_str());
        connTime = epicsTime::getCurrent();

        auto peerCred(std::make_shared<ServerCredentials>());
        peerCred->peer = peerName;
        peerCred->isTLS = isTLS;

#ifdef PVXS_ENABLE_OPENSSL
        if(isTLS) {
            auto ctx = bufferevent_openssl_get_ssl(bev.get());
            assert(ctx);
            ossl::SSLContext::fill_credentials(*peerCred, ctx);
        } else
#endif
        {
            peerCred->method = "anonymous";
        }
        cred = std::move(peerCred);

        {
            // after async connect() to avoid winsock specific race.
            auto fd(bufferevent_getfd(bev.get()));
            int opt = 1;
            if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt))<0) {
                auto err(SOCKERRNO);
                log_warn_printf(io, "Unable to TCP_NODELAY: %d on %d\n", err, fd);
            }
        }

        if(bufferevent_enable(bev.get(), EV_READ|EV_WRITE))
            throw std::logic_error("Unable to enable BEV");

        // start echo timer
        // tcpTimeout(40) -> 15 second echo period
        // bound echo to range [1, 15]
        timeval tmo(totv(std::max(1.0, std::min(15.0, context->effective.tcpTimeout*3.0/8.0))));
        if(event_add(echoTimer.get(), &tmo))
            log_err_printf(io, "Server %s error starting echoTimer\n", peerName.c_str());

        state = Connected;
    }
}

std::shared_ptr<ConnBase> Connection::self_from_this()
{
    return shared_from_this();
}

void Connection::cleanup()
{
    ready = false;

    context->connByAddr.erase(std::make_pair(peerAddr, isTLS));

    if(bev)
        bev.reset();

    if(event_del(echoTimer.get()))
        log_err_printf(io, "Server %s error stopping echoTimer\n", peerName.c_str());

    // return Channels to Searching state
    std::set<std::shared_ptr<Channel>> todo;
    for(auto& pair : pending) {
        if(auto chan = pair.second.lock())
            todo.insert(chan);
    }
    for(auto& pair : chanBySID) {
        if(auto chan = pair.second.lock())
            todo.insert(chan);
    }
    for(auto& pair : creatingByCID) {
        if(auto chan = pair.second.lock())
            todo.insert(chan);
    }

    for(auto& chan : todo) {
        chan->disconnect(chan);
    }

    // Channel::disconnect() should clean
    assert(opByIOID.empty());

    // paranoia
    pending.clear();
    chanBySID.clear();
}

void Connection::handle_CONNECTION_VALIDATION()
{
    log_debug_printf(io, "Server %s begins validation handshake\n", peerName.c_str());

    EvInBuf M(peerBE, segBuf.get(), 16);

    // unused
    //   serverReceiveBufferSize
    //   serverIntrospectionRegistryMaxSize
    M.skip(4u + 2u, __FILE__, __LINE__);

    Size nauth{};
    from_wire(M, nauth);

    std::string selected;

    /* Server list given in reverse order of priority.
     * Old pvAccess* was missing a "break" when looping,
     * so it took the last known plugin.
     */
    for(auto n : range(nauth.size)) {
        (void)n;

        std::string method;
        from_wire(M, method);

        if(method=="ca" || (method=="anonymous" && selected!="ca"))
            selected = method;
#ifdef PVXS_ENABLE_OPENSSL
        else if(isTLS && method=="x509" && context->tls_context.have_certificate())
            selected = method;
#endif
    }

    if(!M.good()) {
        log_err_printf(io, "%s:%d Server %s sends invalid CONNECTION_VALIDATION.  Disconnect...\n",
                       M.file(), M.line(), peerName.c_str());
        bev.reset();
        return;
    }

    if(!selected.empty()) {
        log_debug_printf(io, "Server %s selecting auth '%s'\n", peerName.c_str(), selected.c_str());

    } else {
        selected = "anonymous";
        log_warn_printf(io, "Server %s no supported auth.  try to force '%s'\n", peerName.c_str(), selected.c_str());
    }

    Value cred;
    if(selected=="ca") {
        cred = context->caMethod.cloneEmpty();

        std::vector<char> buffer(256u);

        if(osiGetUserName(&buffer[0], buffer.size()) == osiGetUserNameSuccess) {
            buffer[buffer.size()-1] = '\0';
            cred["user"] = buffer.data();
        } else {
            cred["user"] = "nobody";
        }

        if (gethostname(&buffer[0], buffer.size()) == 0) {
            buffer[buffer.size()-1] = '\0';
            cred["host"] = buffer.data();
        } else {
            cred["host"] = "invalidhost.";
        }

        log_info_printf(io, "Server %s 'ca' auth as %s@%s\n", peerName.c_str(),
                        cred["user"].as<std::string>().c_str(),
                        cred["host"].as<std::string>().c_str());
    }

    {
        (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

        EvOutBuf R(sendBE, txBody.get());

        // serverReceiveBufferSize, not used
        to_wire(R, uint32_t(0x10000));
        // serverIntrospectionRegistryMaxSize, also not used
        to_wire(R, uint16_t(0x7fff));
        // QoS, not used (quality?)
        to_wire(R, uint16_t(0));

        to_wire(R, selected);

        to_wire(R, Value::Helper::desc(cred));
        if(cred)
            to_wire_full(R, cred);
    }
    enqueueTxBody(CMD_CONNECTION_VALIDATION);
}

void Connection::handle_CONNECTION_VALIDATED()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    Status sts{};
    from_wire(M, sts);

    if(!M.good()) {
        log_crit_printf(io, "%s:%d Server %s sends invalid CONNECTION_VALIDATED.  Disconnecting...\n",
                        M.file(), M.line(), peerName.c_str());
        bev.reset();
        return;

    } else if(!sts.isSuccess()) {
        log_err_printf(io, "Server %s refuses auth.  Trying to proceed w/o cred\n", peerName.c_str());

    } else {
        log_debug_printf(io, "Server %s accepts auth%s%s\n", peerName.c_str(),
                         sts.msg.empty() ? "" : " ", sts.msg.c_str());
    }

    ready = true;

    createChannels();

    if(nameserver) {
        log_info_printf(io, "(re)connected to nameserver %s\n", peerName.c_str());
        context->poke();
    }
}

void Connection::handle_CREATE_CHANNEL()
{
    auto rxlen = 8u + evbuffer_get_length(segBuf.get());
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t cid, sid;
    Status sts{};

    from_wire(M, cid);
    from_wire(M, sid);
    from_wire(M, sts);
    // "spec" calls for uint16_t Access Rights here, but pvAccessCPP don't include this (it's useless anyway)

    if(!M.good()) {
        log_crit_printf(io, "%s:%d Server %s sends invalid CREATE_CHANNEL.  Disconnecting...\n",
                        M.file(), M.line(), peerName.c_str());
        bev.reset();
        return;
    }

    std::shared_ptr<Channel> chan;
    {
        auto it = creatingByCID.find(cid);
        if(it==creatingByCID.end() || !(chan = it->second.lock())) {

            if(it!=creatingByCID.end())
                creatingByCID.erase(it);

            if(sts.isSuccess()) {
                // we now have a channel which is no longer interesting.
                log_debug_printf(io, "Server %s disposing of newly stale channel\n", peerName.c_str());

                {
                    (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

                    EvOutBuf R(sendBE, txBody.get());
                    to_wire(R, sid);
                    to_wire(R, cid);
                }
                enqueueTxBody(CMD_DESTROY_CHANNEL);
            }
            return;
        }
        creatingByCID.erase(it);
    }
    chan->statRx += rxlen;

    if(!sts.isSuccess()) {
        // server refuses to create a channel, but presumably responded positively to search

        chan->state = Channel::Searching;
        context->searchBuckets[context->currentBucket].push_back(chan);

        log_warn_printf(io, "Server %s refuses channel to '%s' : %s\n", peerName.c_str(),
                        chan->name.c_str(), sts.msg.c_str());

    } else {
        chan->state = Channel::Active;
        chan->sid = sid;

        chanBySID[sid] = chan;

        log_debug_printf(io, "Server %s active channel to '%s' %u:%u\n", peerName.c_str(),
                         chan->name.c_str(), unsigned(chan->cid), unsigned(chan->sid));

        chan->createOperations();

        auto conns(chan->connectors); // copy list

        struct Connected connEvt(peerName, connTime, cred);
        for(auto& conn : conns) {
            if(!conn->_connected.exchange(true, std::memory_order_relaxed) && conn->_onConn)
                conn->_onConn(connEvt);
        }
    }
}

void Connection::handle_DESTROY_CHANNEL()
{
    uint32_t cid=0, sid=0;
    {
        EvInBuf M(peerBE, segBuf.get(), 16);

        from_wire(M, sid);
        from_wire(M, cid);

        if(!M.good()) {
            log_crit_printf(io, "%s:%d Server %s sends invalid DESTROY_CHANNEL.  Disconnecting...\n",
                            M.file(), M.line(), peerName.c_str());
            bev.reset();
            return;
        }
    }

    std::shared_ptr<Channel> chan;
    {
        auto it = chanBySID.find(sid);
        if(it==chanBySID.end() || !(chan = it->second.lock())) {
            log_debug_printf(io, "Server %s destroys non-existent channel %u:%u\n",
                             peerName.c_str(), unsigned(cid), unsigned(sid));
            return;
        }
    }

    chanBySID.erase(sid);
    chan->disconnect(chan);

    log_debug_printf(io, "Server %s destroys channel '%s' %u:%u\n",
                     peerName.c_str(), chan->name.c_str(), unsigned(cid), unsigned(sid));
}

void Connection::handle_MESSAGE()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t ioid = 0;
    uint8_t mtype = 0;
    std::string msg;

    from_wire(M, ioid);
    from_wire(M, mtype);
    from_wire(M, msg);

    if(!M.good())
        throw std::runtime_error(SB()<<M.file()<<':'<<M.line()<<" Decode error for Message");

    auto it = opByIOID.find(ioid);
    if(it==opByIOID.end()) {
        log_debug_printf(connsetup, "Server %s Message on non-existent ioid\n", peerName.c_str());
        return;
    }
    auto op = it->second.handle.lock();

    Level lvl;
    switch(mtype) {
    case 0:  lvl = Level::Info; break;
    case 1:  lvl = Level::Warn; break;
    case 2:  lvl = Level::Err; break;
    default: lvl = Level::Crit; break;
    }

    log_printf(remote, lvl, "%s : %s\n",
               op && op->chan ? op->chan->name.c_str() : "<dead>", msg.c_str());
}

void Connection::tickEcho()
{
    if(state==Holdoff) {
        log_debug_printf(io, "Server %s holdoff expires\n", peerName.c_str());

        if(event_del(echoTimer.get()))
            log_err_printf(io, "Server %s error Disabling echoTimer\n", peerName.c_str());

        startConnecting();

    }else if(state==Disconnected) {
        // deferred notification of early connect() failure.
        // TODO: avoid a misleading "closed by peer" error
        bevEvent(BEV_EVENT_EOF);

    } else {
        log_debug_printf(io, "Server %s ping\n", peerName.c_str());

        if(!bev)
            return;

        auto tx = bufferevent_get_output(bev.get());

        to_evbuf(tx, Header{CMD_ECHO, 0u, 0u}, sendBE);

        // maybe help reduce latency
        bufferevent_flush(bev.get(), EV_WRITE, BEV_FLUSH);

        statTx += 8;
    }
}

void Connection::tickEchoS(evutil_socket_t fd, short evt, void *raw)
{
    try {
        static_cast<Connection*>(raw)->tickEcho();
    }catch(std::exception& e){
        log_exc_printf(io, "Unhandled error in echo timer callback: %s\n", e.what());
    }
}

} // namespace client
} // namespace pvxs
