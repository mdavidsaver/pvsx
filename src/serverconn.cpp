/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <limits>
#include <system_error>
#include <utility>

#include <osiSock.h>
#include <epicsGuard.h>
#include <epicsAssert.h>

#include <pvxs/log.h>
#include "serverconn.h"

// limit on size of TX buffer above which we suspend RX.
// defined as multiple of OS socket TX buffer size
static constexpr size_t tcp_tx_limit_mult = 2u;

namespace pvxs {
namespace impl {
DEFINE_INST_COUNTER(ServerChannelControl);
DEFINE_INST_COUNTER(ServerChan);
DEFINE_INST_COUNTER(ServerConn);
DEFINE_INST_COUNTER(ServerSource);
}

std::set<std::string> PeerCredentials::roles() const
{
    std::set<std::string> ret;
    osdGetRoles(account, ret);
    return ret;
}

std::ostream& operator<<(std::ostream& strm, const PeerCredentials& cred)
{
    if(cred.isTLS)
        strm<<"TLS ";
    strm<<cred.method;
    if(!cred.authority.empty())
        strm<<":"<<cred.authority;
    strm<<"/"<<cred.account<<"@"<<cred.peer;
    return strm;
}

namespace server {

DEFINE_INST_COUNTER2(Server::Pvt, ServerPvt);

}} // namespace pvxs::server

namespace pvxs {namespace impl {

// message related to client state and errors
DEFINE_LOGGER(connsetup, "pvxs.tcp.setup");
// related to low level send/recv
DEFINE_LOGGER(connio, "pvxs.tcp.io");

DEFINE_LOGGER(remote, "pvxs.remote.log");

ServerConn::ServerConn(ServIface* iface, evutil_socket_t sock, struct sockaddr *peer, int socklen)
    :ConnBase(false, iface->server->effective.sendBE(),
              evbufferevent(__FILE__, __LINE__,
                bufferevent_socket_new(iface->server->acceptor_loop.base, sock, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS)
              ),
              SockAddr(peer))
    ,iface(iface)
    ,tcp_tx_limit(evsocket::get_buffer_size(sock, true) * tcp_tx_limit_mult)
{
    log_debug_printf(connio, "Client %s connects%s, RX readahead %zu TX limit %zu\n",
                     peerName.c_str(), iface->isTLS ? " TLS" : "", readahead, tcp_tx_limit);
    {
        int opt = 1;
        if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt))<0) {
            auto err(SOCKERRNO);
            log_warn_printf(connio, "Unable to TCP_NODELAY: %d on %d\n", err, sock);
        }
    }

#ifdef PVXS_ENABLE_OPENSSL
    if(iface->isTLS) {
        assert(iface->server->tls_context);
        auto ctx(SSL_new(iface->server->tls_context.ctx));
        if(!ctx)
            throw ossl::SSLError("SSL_new()");
        auto rawconn = bev.release();
        // BEV_OPT_CLOSE_ON_FREE will free on error
        evbufferevent tlsconn(__FILE__, __LINE__,
                              bufferevent_openssl_filter_new(iface->server->acceptor_loop.base,
                                                             rawconn,
                                                             ctx,
                                                             BUFFEREVENT_SSL_ACCEPTING,
                                                             BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS));
        bev = std::move(tlsconn);

        // added with libevent 2.2.1-alpha
        //(void)bufferevent_ssl_set_flags(bev.get(), BUFFEREVENT_SSL_DIRTY_SHUTDOWN);
        // deprecated, but not yet removed
        bufferevent_openssl_set_allow_dirty_shutdown(bev.get(), 1);
    }
#endif
    {
        auto cred(std::make_shared<server::ClientCredentials>());
        cred->peer = peerName;
        cred->iface = iface->name;
        // paranoia placeholder prior to handle_CONNECTION_VALIDATION()
        cred->method = cred->account = "anonymous";
        this->cred = std::move(cred);
    }

    bufferevent_setcb(bev.get(), &bevReadS, &bevWriteS, &bevEventS, this);

    timeval tmo(totv(iface->server->effective.tcpTimeout));
    bufferevent_set_timeouts(bev.get(), &tmo, &tmo);

    auto tx = bufferevent_get_output(bev.get());

    std::vector<uint8_t> buf(128);

    // queue connection validation message
    {
        VectorOutBuf M(sendBE, buf);
        to_wire(M, Header{pva_ctrl_msg::SetEndian, pva_flags::Control|pva_flags::Server, 0});

        auto save = M.save();
        M.skip(8, __FILE__, __LINE__); // placeholder for header
        auto bstart = M.save();

        // serverReceiveBufferSize, not used
        to_wire(M, uint32_t(0x10000));
        // serverIntrospectionRegistryMaxSize, also not used
        to_wire(M, uint16_t(0x7fff));

        /* list given in reverse order of priority.
         * Old pvAccess* was missing a "break" when looping,
         * so it took the last known plugin.
         */
        to_wire(M, Size{iface->isTLS ? 3u : 2u});
        to_wire(M, "anonymous");
        to_wire(M, "ca");
        if(iface->isTLS)
            to_wire(M, "x509");
        auto bend = M.save();

        FixedBuf H(sendBE, save, 8);
        to_wire(H, Header{CMD_CONNECTION_VALIDATION, pva_flags::Server, uint32_t(bend-bstart)});

        assert(M.good() && H.good());

        if(evbuffer_add(tx, buf.data(), M.save()-buf.data()))
            throw BAD_ALLOC();

        statTx += M.save()-buf.data();
    }

    if(bufferevent_enable(bev.get(), EV_READ|EV_WRITE))
        throw std::logic_error("Unable to enable BEV");
}

ServerConn::~ServerConn()
{}

const std::shared_ptr<ServerChan>& ServerConn::lookupSID(uint32_t sid)
{
    auto it = chanBySID.find(sid);
    if(it==chanBySID.end()) {
        static decltype (it->second) empty{};
        return empty;
        //throw std::runtime_error(SB()<<"Client "<<peerName<<" non-existent SID "<<sid);
    }
    return it->second;
}

void ServerConn::handle_ECHO()
{
    // Client requests echo as a keep-alive check

    auto tx = bufferevent_get_output(bev.get());
    uint32_t len = evbuffer_get_length(segBuf.get());

    to_evbuf(tx, Header{CMD_ECHO, pva_flags::Server, len}, sendBE);

    auto err = evbuffer_add_buffer(tx, segBuf.get());
    assert(!err);

    // maybe help reduce latency
    bufferevent_flush(bev.get(), EV_WRITE, BEV_FLUSH);

    statTx += 8u + len;
}

static
void auth_complete(ServerConn *self, const Status& sts)
{
    (void)evbuffer_drain(self->txBody.get(), evbuffer_get_length(self->txBody.get()));

    {
        EvOutBuf M(self->sendBE, self->txBody.get());
        to_wire(M, sts);
    }

    self->enqueueTxBody(CMD_CONNECTION_VALIDATED);

    log_debug_printf(connsetup, "%s Auth complete with %d\n", self->peerName.c_str(), sts.code);
}

void ServerConn::handle_CONNECTION_VALIDATION()
{
    // Client begins (restarts?) Auth handshake

    EvInBuf M(peerBE, segBuf.get(), 16);

    std::string selected;
    {
        M.skip(4+2+2, __FILE__, __LINE__); // ignore unused buffer, introspection size, and QoS
        from_wire(M, selected);

        Value auth;
        from_wire_type_value(M, rxRegistry, auth);

        if(!M.good()) {
            log_err_printf(connio, "%s:%d Client %s Truncated/Invalid ConnValid from client\n",
                           M.file(), M.line(), peerName.c_str());
            bev.reset();
            return;

        } else {
            log_debug_printf(connsetup, "Client %s authenticates using %s and %s\n",
                       peerName.c_str(), selected.c_str(),
                       std::string(SB()<<auth).c_str());

            auto C(std::make_shared<server::ClientCredentials>(*cred));
            C->isTLS = iface->isTLS;

            if(selected=="ca") {
                auth["user"].as<std::string>([&C, &selected](const std::string& user) {
                    C->method = selected;
                    C->account = user;
                });
            }
#ifdef PVXS_ENABLE_OPENSSL
            else if(iface->isTLS && selected=="x509" && bev) {
                auto ctx = bufferevent_openssl_get_ssl(bev.get());
                assert(ctx);
                ossl::SSLContext::fill_credentials(*C, ctx);
            }
#endif
            if(C->method.empty()) {
                C->account = C->method = "anonymous";
            }
            C->raw = auth;

            cred = std::move(C);
        }
    }

    if(selected!="ca" && selected!="anonymous" && selected!="x509") {
        log_debug_printf(connsetup, "Client %s selects unadvertised auth \"%s\"", peerName.c_str(), selected.c_str());
        auth_complete(this, Status{Status::Error, "Client selects unadvertised auth"});
        return;

    } else {
        log_debug_printf(connsetup, "Client %s selects auth \"%s\" as \"%s\"\n",
                         peerName.c_str(), cred->method.c_str(), cred->account.c_str());
    }

    // remainder of segBuf is payload w/ credentials

    // No practical way to handle auth failure.
    // So we accept all credentials, but may not grant rights.
    auth_complete(this, Status{Status::Ok});
}

void ServerConn::handle_AUTHNZ()
{
    // ignored (so far no auth plugin actually uses)
}

void ServerConn::handle_PUT_GET()
{}

void ServerConn::handle_CANCEL_REQUEST()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t sid=0, ioid=0;
    from_wire(M, sid);
    from_wire(M, ioid);
    if(!M.good())
        throw std::runtime_error(SB()<<M.file()<<':'<<M.line()<<" Error decoding DestroyOp");

    auto it = opByIOID.find(ioid);
    if(it==opByIOID.end()) {
        log_warn_printf(connsetup, "Client %s Cancel of non-existent Op %u\n", peerName.c_str(), unsigned(ioid));
        return;
    }

    const auto& op = it->second;
    auto chan = op->chan.lock();
    if(!chan || chan->sid!=sid) {
        log_err_printf(connsetup, "Client %s Cancel inconsistent Op\n", peerName.c_str());
        return;
    }

    if(op->state==ServerOp::Executing) {
        op->state = ServerOp::Idle;

        if(op->onCancel)
            op->onCancel();

    } else {
        // an allowed race
        log_debug_printf(connsetup, "Client %s Cancel of non-executing Op\n", peerName.c_str());
    }
}

void ServerConn::handle_DESTROY_REQUEST()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t sid=0, ioid=0;
    from_wire(M, sid);
    from_wire(M, ioid);
    if(!M.good())
        throw std::runtime_error(SB()<<M.file()<<':'<<M.line()<<" Error decoding DestroyOp");

    auto& chan = lookupSID(sid);
    auto it = opByIOID.find(ioid);

    if(!chan || it==opByIOID.end() || 1!=chan->opByIOID.erase(ioid)) {
        log_debug_printf(connsetup, "Client %s can't destroy non-existent op %u:%u\n",
                   peerName.c_str(), unsigned(sid), unsigned(ioid));

    }

    if(it!=opByIOID.end()) {
        auto op = it->second;
        opByIOID.erase(it);
        op->cleanup();
    }
}

void ServerConn::handle_MESSAGE()
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
        log_debug_printf(connsetup, "Client %s Message on non-existent ioid\n", peerName.c_str());
        return;
    }
    auto chan = it->second->chan.lock();

    Level lvl;
    switch(mtype) {
    case 0:  lvl = Level::Info; break;
    case 1:  lvl = Level::Warn; break;
    case 2:  lvl = Level::Err; break;
    default: lvl = Level::Crit; break;
    }

    log_printf(remote, lvl, "%s : %s\n",
               chan ? chan->name.c_str() : "<dead>", msg.c_str());
}

std::shared_ptr<ConnBase> ServerConn::self_from_this()
{
    return shared_from_this();
}

// see also ServerChannel_shutdown()
/* reached from:
 * 1. connection close
 */
void ServerConn::cleanup()
{
    log_debug_printf(connsetup, "Client %s Cleanup TCP Connection\n", peerName.c_str());

    iface->server->connections.erase(this);

    // grab maps before cleanup()s would modify
    auto ops(std::move(opByIOID));
    auto chans(std::move(chanBySID));

    for(auto& op : ops) {
        op.second->cleanup();
    }
    for(auto& pair : chans) {
        pair.second->cleanup();
    }
}

void ServerConn::bevEvent(short events)
{
#ifdef PVXS_ENABLE_OPENSSL
    if((events & (BEV_EVENT_ERROR|BEV_EVENT_EOF)) && iface->isTLS && bev) {
        while(auto err = bufferevent_get_openssl_error(bev.get())) {
            log_err_printf(connio, "TLS Error (0x%lx) %s\n",
                           err, ERR_reason_error_string(err));
        }
    }
#endif
    ConnBase::bevEvent(events);
}

void ServerConn::bevRead()
{
    ConnBase::bevRead();

    if(bev) {
        auto tx = bufferevent_get_output(bev.get());

        if(evbuffer_get_length(tx)>=tcp_tx_limit) {
            // write buffer "full".  stop reading until it drains
            // TODO configure
            (void)bufferevent_disable(bev.get(), EV_READ);
            bufferevent_setwatermark(bev.get(), EV_WRITE, tcp_tx_limit/2, 0);
            log_debug_printf(connio, "%s suspend READ\n", peerName.c_str());
        }
    }
}

void ServerConn::bevWrite()
{
    log_debug_printf(connio, "%s process backlog\n", peerName.c_str());

    auto tx = bufferevent_get_output(bev.get());
    // handle pending monitors

    while(!backlog.empty() && evbuffer_get_length(tx)<tcp_tx_limit) {
        auto fn = std::move(backlog.front());
        backlog.pop_front();

        fn();
    }

    // TODO configure
    if(evbuffer_get_length(tx)<tcp_tx_limit) {
        (void)bufferevent_enable(bev.get(), EV_READ);
        bufferevent_setwatermark(bev.get(), EV_WRITE, 0, 0);
        log_debug_printf(connio, "%s resume READ\n", peerName.c_str());
    }
}


ServIface::ServIface(const SockAddr &addr, server::Server::Pvt *server, bool fallback, bool isTLS)
    :server(server)
    ,isTLS(isTLS)
    ,bind_addr(addr)
{
    server->acceptor_loop.assertInLoop();
    auto orig_port = bind_addr.port();

    // try to bind to requested port, then fallback to a random port
    while(true) {

        sock = evsocket(bind_addr.family(), SOCK_STREAM, 0);

        if(evutil_make_listen_socket_reuseable(sock.sock))
            log_warn_printf(connsetup, "Unable to make socket reusable%s", "\n");

        try {
            sock.bind(bind_addr);
            // semantics of SO_REUSEADDR on *nix allow multiple bind() to success.
            // however, only one listen() will succeed

            // added in libevent 2.1.1
#ifndef LEV_OPT_DISABLED
#  define LEV_OPT_DISABLED 0
#endif

            const int backlog = 4;
            auto list(evconnlistener_new(server->acceptor_loop.base, onConnS, this, LEV_OPT_DISABLED|LEV_OPT_CLOSE_ON_EXEC, backlog, sock.sock));
            if(!list) {
                int err = evutil_socket_geterror(sock);
                throw std::system_error(err, std::system_category());
            }
            listener = evlisten(__FILE__, __LINE__, list);

            if(!LEV_OPT_DISABLED)
                evconnlistener_disable(listener.get());

        } catch(std::system_error& e) {
            if(fallback && (e.code().value()==SOCK_EADDRINUSE || e.code().value()==SOCK_EACCES)) {
                log_debug_printf(connsetup, "Address %s in use or not permitted: %s\n",
                                 bind_addr.tostring().c_str(),
                                 e.what());
                bind_addr.setPort(0);
                fallback = false;
                continue;
            }
            log_err_printf(connsetup, "Bind to %s fails%s with %d\n",
                           bind_addr.tostring().c_str(),
                           fallback ? "" : " after fallback",
                           e.code().value());
            throw;
        }
        break;
    }

    name = bind_addr.tostring();

    if(orig_port && bind_addr.port() != orig_port) {
        log_warn_printf(connsetup, "Server unable to bind port %u, falling back to %s\n", orig_port, name.c_str());
    }
}

void ServIface::onConnS(struct evconnlistener *listener, evutil_socket_t sock, struct sockaddr *peer, int socklen, void *raw)
{
    auto self = static_cast<ServIface*>(raw);
    try {
        auto conn(std::make_shared<ServerConn>(self, sock, peer, socklen));
        self->server->connections[conn.get()] = std::move(conn);
    }catch(std::exception& e){
        log_exc_printf(connsetup, "Interface %s Unhandled error in accept callback: %s\n", self->name.c_str(), e.what());
        evutil_closesocket(sock);
    }
}

ServerOp::~ServerOp()
{
    // cleanup() should have happened already (from tcp worker)
    // this check may run from any thread, but at this point it should not matter.
    assert(state==Dead);
}

/* reached from:
 * 1. connection close
 * 2. DESTROY_CHANNEL
 * 3. DESTROY_REQUEST
 * 4. individual op DESTROY
 * 5. local user calls ServerChannelControl::close()
 */
void ServerOp::cleanup()
{
    if(state==ServerOp::Dead)
        return;

    if(state==ServerOp::Executing && onCancel) {
        auto fn(std::move(onCancel));
        fn();
    }

    state = ServerOp::Dead;

    onCancel = nullptr;
    auto closer(std::move(onClose));
    bool notify = closer.operator bool();

    if(auto ch = chan.lock()) {
        ch->opByIOID.erase(ioid);

        if(auto conn = ch->conn.lock()) {
            conn->opByIOID.erase(ioid);

            if(notify) {
                conn->iface->server->acceptor_loop.dispatch([closer](){
                    closer("");
                });
                notify = false;
            }
        }
    }

    if(notify)
        closer("");
}

}} // namespace pvxs::impl
