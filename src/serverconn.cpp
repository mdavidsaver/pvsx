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

// Amount of following messages which we allow to be read while
// processing the current message.  Avoids some extra recv() calls,
// at the price of maybe extra copying.
static const size_t tcp_readahead = 0x1000;

namespace pvxsimpl {

// message related to client state and errors
DEFINE_LOGGER(connsetup, "tcp.setup");
// related to low level send/recv
DEFINE_LOGGER(connio, "tcp.io");

ServerConn::ServerConn(ServIface* iface, evutil_socket_t sock, struct sockaddr *peer, int socklen)
    :iface(iface)
    ,peerAddr(peer, socklen)
    ,peerName(peerAddr.tostring())
    ,bev(bufferevent_socket_new(iface->server->acceptor_loop.base, sock, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS))
    ,peerBE(true) // arbitrary choice, default should be overwritten before use
    ,expectSeg(false)
    ,segCmd(0xff)
    ,segBuf(evbuffer_new())
    ,txBody(evbuffer_new())
    ,nextSID(0)
{
    log_printf(connsetup, PLVL_DEBUG, "Client %s connects\n", peerName.c_str());

    bufferevent_setcb(bev.get(), &bevReadS, &bevWriteS, &bevEventS, this);
    // initially wait for at least a header
    bufferevent_setwatermark(bev.get(), EV_READ, 8, tcp_readahead);

    timeval timo = {30, 0};
    bufferevent_set_timeouts(bev.get(), &timo, &timo);

    auto tx = bufferevent_get_output(bev.get());

    std::vector<uint8_t> buf(128);
    const bool be = EPICS_BYTE_ORDER == EPICS_ENDIAN_BIG;

    // queue connection validation message
    {
        uint8_t flags = be ? pva_flags::MSB : 0;
        flags |= pva_flags::Server;

        VectorOutBuf M(be, buf);
        to_wire(M, Header{pva_ctrl_msg::SetEndian, pva_flags::Control|pva_flags::Server, 0});

        auto save = M.save();
        M.skip(8); // placeholder for header
        auto bstart = M.save();

        // serverReceiveBufferSize, not used
        to_wire(M, uint32_t(0x10000));
        // serverIntrospectionRegistryMaxSize, also not used
        to_wire(M, uint16_t(0x7fff));
        to_wire(M, Size{2});
        to_wire(M, "anonymous");
        to_wire(M, "ca");
        auto bend = M.save();

        FixedBuf<uint8_t> H(be, save, 8);
        to_wire(H, Header{pva_app_msg::ConnValid, pva_flags::Server, uint32_t(bend-bstart)});

        assert(M.good() && H.good());

        if(evbuffer_add(tx, buf.data(), M.save()-buf.data()))
            throw std::bad_alloc();
    }

    if(bufferevent_enable(bev.get(), EV_READ|EV_WRITE))
        throw std::logic_error("Unable to enable BEV");
}

ServerConn::~ServerConn()
{}


void ServerConn::handle_Echo()
{
    // Client requests echo as a keep-alive check

    auto tx = bufferevent_get_output(bev.get());
    uint32_t len = evbuffer_get_length(segBuf.get());

    const bool be = EPICS_BYTE_ORDER == EPICS_ENDIAN_BIG;
    to_evbuf(tx, Header{pva_app_msg::Echo, pva_flags::Server, len}, be);

    auto err = evbuffer_add_buffer(tx, segBuf.get());
    assert(!err);

    // maybe help reduce latency
    bufferevent_flush(bev.get(), EV_WRITE, BEV_FLUSH);
}

static
void auth_complete(ServerConn *self, const Status& sts)
{
    const bool be = EPICS_BYTE_ORDER==EPICS_ENDIAN_BIG;
    (void)evbuffer_drain(self->txBody.get(), evbuffer_get_length(self->txBody.get()));

    {
        EvOutBuf M(be, self->txBody.get());
        to_wire(M, sts);
    }

    auto tx = bufferevent_get_output(self->bev.get());
    to_evbuf(tx, Header{pva_app_msg::ConnValidated,
                        pva_flags::Server,
                        uint32_t(evbuffer_get_length(self->txBody.get()))},
             be);
    auto err = evbuffer_add_buffer(tx, self->txBody.get());
    assert(!err);

    log_printf(connsetup, PLVL_DEBUG, "%s Auth complete with %d\n", self->peerName.c_str(), sts.code);
}

void ServerConn::handle_ConnValid()
{
    // Client begins (restarts?) Auth handshake

    EvInBuf M(peerBE, segBuf.get(), 16);

    std::string selected;
    {
        M.skip(6); // ignore unused buffer and introspection size
        uint16_t qos;
        from_wire(M, qos);
        from_wire(M, selected);

        if(!M.good()) {
            log_printf(connio, PLVL_ERR, "Client %s Truncated/Invalid ConnValid from client", peerName.c_str());
            bev.reset();
            return;

        }
    }

    if(selected!="ca" && selected!="anonymous") {
        log_printf(connsetup, PLVL_DEBUG, "Client %s selects unadvertised auth \"%s\"", peerName.c_str(), selected.c_str());
        auth_complete(this, Status{Status::Error, "Client selects unadvertised auth"});
        return;

    } else {
        log_printf(connsetup, PLVL_DEBUG, "Client %s selects auth \"%s\"", peerName.c_str(), selected.c_str());
    }

    // remainder of segBuf is payload w/ credentials

    // TODO actually check credentials
    auth_complete(this, Status{Status::Ok});
}

void ServerConn::handle_AuthZ()
{
    // ignored (so far no auth plugin actually uses)
}

void ServerConn::handle_Search()
{}

void ServerConn::handle_CreateChan()
{
    const bool be = EPICS_BYTE_ORDER==EPICS_ENDIAN_BIG;

    EvInBuf M(peerBE, segBuf.get(), 16);

    auto G(iface->server->sourcesLock.lockReader());

    uint16_t count = 0;
    from_wire(M, count);
    for(auto i : range(count)) {
        (void)i;
        uint32_t cid = -1, sid = -1;
        server::Source::Create op{peerName};
        from_wire(M, cid);
        from_wire(M, op.name);

        if(!M.good())
            break;

        Status sts{Status::Ok};

        bool claimed = false;
        try {
            if(chanByCID.size()==0xffffffff || chanBySID.size()==0xffffffff) {
                sts.code = Status::Error;
                sts.msg = "Too many Server channels";
                sts.trace = "pvx:serv:chanidoverflow:";
            }

            if(sts.isSuccess() && chanByCID.find(cid)!=chanByCID.end()) {
                sts.code = Status::Fatal;
                sts.msg = "Client reuses existing CID";
                sts.trace = "pvx:serv:dupcid:";
            }

            std::unique_ptr<server::Handler> handler;
            if(sts.isSuccess() && !op.name.empty()) {
                for(auto& pair : iface->server->sources) {
                    try {
                        handler = pair.second->onCreate(op);
                        if(handler)
                            break;
                    }catch(std::exception& e){
                        log_printf(connsetup, PLVL_ERR, "Client %s Unhandled error in onCreate %s,%d %s : %s\n", peerName.c_str(),
                                   pair.first.second.c_str(), pair.first.first,
                                   typeid(&e).name(), e.what());
                    }
                }
            }

            if(sts.isSuccess() && handler) {
                do {
                    sid = nextSID++;
                } while(chanBySID.find(sid)!=chanBySID.end());

                auto pair = chanBySID.emplace(std::piecewise_construct,
                                              std::make_tuple(sid),
                                              std::make_tuple(this, sid, cid, op.name, std::move(handler)));
                auto pair2 = chanByCID.emplace(cid, &pair.first->second);
                assert(!!pair.second && !!pair2.second); // we've already checked for a duplicate
                claimed = true;
            }
        }catch(std::exception& e){
            log_printf(connsetup, PLVL_ERR, "Client %s Unhandled error in onCreate %s : %s\n", peerName.c_str(),
                       typeid(&e).name(), e.what());
            sts.code = Status::Fatal;
            sts.msg = e.what();
            sts.trace = "pvx:serv:internal:";
        }

        {
            if(sts.isSuccess() && !claimed) {
                sts.code = Status::Fatal;
                sts.msg = "Unable to create Channel";
                sts.trace = "pvx:serv:nosource:";
            }

            (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

            EvOutBuf R(be, txBody.get());
            to_wire(R, cid);
            to_wire(R, sid);
            to_wire(R, sts);
            // "spec" calls for uint16_t Access Rights here, but pvAccessCPP don't include this (it's useless anyway)
            if(!R.good()) {
                M.fault();
                log_printf(connio, PLVL_ERR, "Client %s Encode error in CreateChan\n", peerName.c_str());
                break;
            }
        }

        auto tx = bufferevent_get_output(bev.get());
        to_evbuf(tx, Header{pva_app_msg::CreateChan,
                            pva_flags::Server,
                            uint32_t(evbuffer_get_length(txBody.get()))},
                 be);
        auto err = evbuffer_add_buffer(tx, txBody.get());
        assert(!err);
    }

    if(!M.good()) {
        log_printf(connio, PLVL_ERR, "Client %s Decode error in CreateChan\n", peerName.c_str());
        bev.reset();
    }
}

void ServerConn::handle_DestroyChan()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t sid=-1, cid=-1;

    from_wire(M, sid);
    from_wire(M, cid);

    auto it = chanBySID.find(sid);
    if(M.good() && it!=chanBySID.end()) {
        auto& chan = it->second;

        if(chan.cid!=cid) {
            log_printf(connsetup, PLVL_DEBUG, "Client %s provides incorrect CID with DestroyChan sid=%d cid=%d!=%d '%s'\n", peerName.c_str(),
                       unsigned(sid), unsigned(chan.cid), unsigned(cid), chan.name.c_str());
        }

        auto n = chanByCID.erase(chan.cid);
        assert(n==1);

        chanBySID.erase(it);
        // ServerChannel is delete'd

        {
            auto tx = bufferevent_get_output(bev.get());
            constexpr bool be = EPICS_BYTE_ORDER==EPICS_ENDIAN_BIG;
            EvOutBuf R(be, tx);
            to_wire(R, Header{pva_app_msg::DestroyChan, pva_flags::Server, 8});
            // yes, CID and SID really are reversed from from the Request
            to_wire(R, cid);
            to_wire(R, sid);
        }

    } else {
        log_printf(connsetup, PLVL_DEBUG, "Client %s DestroyChan non-existant sid=%d cid=%d\n", peerName.c_str(),
                   unsigned(sid), unsigned(cid));
    }

    if(!M.good())
        bev.reset();
}

void ServerConn::handle_GetOp()
{}

void ServerConn::handle_PutOp()
{}

void ServerConn::handle_RPCOp()
{}

void ServerConn::handle_PutGetOp()
{}

void ServerConn::handle_CancelOp()
{}

void ServerConn::handle_DestroyOp()
{}

void ServerConn::handle_Introspect()
{}

void ServerConn::handle_Message()
{}


void ServerConn::cleanup()
{
    log_printf(connsetup, PLVL_DEBUG, "Client %s Cleanup TCP Connection\n", peerName.c_str());

    // remove myself from connections list
    decltype (iface->connections) trash;
    for (auto it = iface->connections.begin(), end = iface->connections.end(); it!=end; ++it) {
        if((&*it)==this) {
            trash.splice(trash.end(), iface->connections, it);
            break;
        }
    }
    assert(!trash.empty());
    // delete this
}

void ServerConn::bevEvent(short events)
{
    if(events&(BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT)) {
        if(events&BEV_EVENT_ERROR) {
            int err = EVUTIL_SOCKET_ERROR();
            const char *msg = evutil_socket_error_to_string(err);
            log_printf(connio, PLVL_ERR, "Client %s connection closed with socket error %d : %s\n", peerName.c_str(), err, msg);
        }
        if(events&BEV_EVENT_EOF) {
            log_printf(connio, PLVL_DEBUG, "Client %s connection closed by peer\n", peerName.c_str());
        }
        if(events&BEV_EVENT_TIMEOUT) {
            log_printf(connio, PLVL_WARN, "Client %s connection timeout\n", peerName.c_str());
        }
        bev.reset();
    }

    if(!bev)
        cleanup();
}

void ServerConn::bevRead()
{
    auto rx = bufferevent_get_input(bev.get());

    while(bev && evbuffer_get_length(rx)>=8) {
        uint8_t header[8];

        auto ret = evbuffer_copyout(rx, header, sizeof(header));
        assert(ret==sizeof(header)); // previously verified

        if(header[0]!=0xca || header[1]==0 || (header[2]&pva_flags::Server)) {
            log_hex_printf(connio, PLVL_ERR, header, sizeof(header), "Client %s Protocol decode fault.  Force disconnect.\n", peerName.c_str());
            bev.reset();
            break;
        }
        log_hex_printf(connio, PLVL_DEBUG, header, sizeof(header), "Client %s Receive header\n", peerName.c_str());

        if(header[2]&pva_flags::Control) {
            // Control messages are not actually useful
            evbuffer_drain(rx, 8);
            continue;
        }
        // application message

        peerBE = header[2]&pva_flags::MSB;

        // a bit verbose :P
        FixedBuf<uint8_t> L(peerBE, header+4, 4);
        uint32_t len = 0;
        from_wire(L, len);
        assert(L.good());

        if(evbuffer_get_length(rx)-8 < len) {
            // wait for complete payload
            // and some additional if available
            size_t readahead = len;
            if(readahead < std::numeric_limits<size_t>::max()-tcp_readahead)
                readahead += tcp_readahead;
            bufferevent_setwatermark(bev.get(), EV_READ, len, readahead);
            break;
        }

        evbuffer_drain(rx, 8);
        {
            unsigned n = evbuffer_remove_buffer(rx, segBuf.get(), len);
            assert(n==len); // we know rx buf contains the entire body
        }

        // so far we do not use segmentation to support incremental processing
        // of long messages.  We instead accumulate all segments of a message
        // prior to parsing.

        auto seg = header[2]&pva_flags::SegMask;

        bool continuation = seg&pva_flags::SegLast; // true for mid or last.  false for none for first
        if((continuation ^ expectSeg) || (continuation && header[3]!=segCmd)) {
            log_printf(connio, PLVL_CRIT, "Client %s Peer segmentation violation %c%c 0x%02x==0x%02x\n", peerName.c_str(),
                       expectSeg?'Y':'N', continuation?'Y':'N',
                       segCmd, header[3]);
            bev.reset();
            break;
        }

        if(!seg || seg==pva_flags::SegFirst) {
            expectSeg = true;
            segCmd = header[3];
        }

        if(!seg || seg==pva_flags::SegLast) {
            expectSeg = false;

            // ready to process segBuf
            switch(segCmd) {
            default:
                log_printf(connio, PLVL_DEBUG, "Client %s Ignore unexpected command 0x%02x\n", peerName.c_str(), segCmd);
                evbuffer_drain(segBuf.get(), evbuffer_get_length(segBuf.get()));
                break;
#define CASE(Op) case pva_app_msg::Op: handle_##Op(); break
                CASE(Echo);
                CASE(ConnValid);
                CASE(Search);
                CASE(AuthZ);

                CASE(CreateChan);
                CASE(DestroyChan);

                CASE(GetOp);
                CASE(PutOp);
                CASE(PutGetOp);
                CASE(RPCOp);
                CASE(CancelOp);
                CASE(DestroyOp);
                CASE(Introspect);

                CASE(Message);
#undef CASE
            }
            // handlers may be cleared bev to force disconnect

            // silently drain any unprocessed body (forward compatibility)
            if(auto n = evbuffer_get_length(segBuf.get()))
                evbuffer_drain(segBuf.get(), n);

            // wait for next header
            bufferevent_setwatermark(bev.get(), EV_READ, 8, tcp_readahead);
        }
    }

    if(!bev) {
        cleanup();

    } else if(auto tx = bufferevent_get_output(bev.get())) {
        if(evbuffer_get_length(tx)>=0x100000) {
            // write buffer "full".  stop reading until it drains
            // TODO configure
            (void)bufferevent_disable(bev.get(), EV_READ);
            bufferevent_setwatermark(bev.get(), EV_WRITE, 0x100000/2, 0);
        }
    }
}

void ServerConn::bevWrite()
{
    (void)bufferevent_enable(bev.get(), EV_READ);
    bufferevent_setwatermark(bev.get(), EV_WRITE, 0, 0);
}

void ServerConn::bevEventS(struct bufferevent *bev, short events, void *ptr)
{
    auto conn = static_cast<ServerConn*>(ptr);
    try {
        conn->bevEvent(events);
    }catch(std::exception& e){
        log_printf(connsetup, PLVL_CRIT, "Client %s Unhandled error in bev event callback: %s\n", conn->peerName.c_str(), e.what());
        static_cast<ServerConn*>(ptr)->cleanup();
    }
}

void ServerConn::bevReadS(struct bufferevent *bev, void *ptr)
{
    auto conn = static_cast<ServerConn*>(ptr);
    try {
        conn->bevRead();
    }catch(std::exception& e){
        log_printf(connsetup, PLVL_CRIT, "Client %s Unhandled error in bev read callback: %s\n", conn->peerName.c_str(), e.what());
        static_cast<ServerConn*>(ptr)->cleanup();
    }
}

void ServerConn::bevWriteS(struct bufferevent *bev, void *ptr)
{
    auto conn = static_cast<ServerConn*>(ptr);
    try {
        conn->bevWrite();
    }catch(std::exception& e){
        log_printf(connsetup, PLVL_CRIT, "Client %s Unhandled error in bev write callback: %s\n", conn->peerName.c_str(), e.what());
        static_cast<ServerConn*>(ptr)->cleanup();
    }
}

ServIface::ServIface(const std::string& addr, unsigned short port, server::Server::Pvt *server)
    :server(server)
    ,bind_addr(AF_INET, addr.c_str(), port)
    ,sock(AF_INET, SOCK_STREAM, 0)
{
    server->acceptor_loop.assertInLoop();

    // try to bind to requested port, then fallback to a random port
    while(true) {
        try {
            sock.bind(bind_addr);
        } catch(std::system_error& e) {
            if(e.code().value()==SOCK_EADDRINUSE && bind_addr.port()!=0) {
                bind_addr.setPort(0);
                continue;
            }
            throw;
        }
        break;
    }

    name = bind_addr.tostring();

    const int backlog = 4;
    listener = evlisten(evconnlistener_new(server->acceptor_loop.base, onConnS, this, LEV_OPT_DISABLED, backlog, sock.sock));
}

void ServIface::onConnS(struct evconnlistener *listener, evutil_socket_t sock, struct sockaddr *peer, int socklen, void *raw)
{
    auto self = static_cast<ServIface*>(raw);
    try {
        if(peer->sa_family!=AF_INET) {
            log_printf(connsetup, PLVL_CRIT, "Interface %s Rejecting !ipv4 client\n", self->name.c_str());
            evutil_closesocket(sock);
            return;
        }
        self->connections.emplace_back(self, sock, peer, socklen);
    }catch(std::exception& e){
        log_printf(connsetup, PLVL_CRIT, "Interface %s Unhandled error in accept callback: %s\n", self->name.c_str(), e.what());
        evutil_closesocket(sock);
    }
}

} // namespace pvxsimpl