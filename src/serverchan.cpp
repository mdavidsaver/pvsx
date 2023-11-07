/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <stdexcept>
#include <cassert>

#include "pvxs/log.h"
#include "serverconn.h"

namespace pvxs {namespace impl {

// message related to client state and errors
DEFINE_LOGGER(connsetup, "pvxs.tcp.setup");
// related to low level send/recv
DEFINE_LOGGER(connio, "pvxs.tcp.io");

DEFINE_LOGGER(serversearch, "pvxs.server.search");

ServerChan::ServerChan(const std::shared_ptr<ServerConn> &conn,
                       uint32_t sid,
                       uint32_t cid,
                       const std::string &name)
    :conn(conn)
    ,sid(sid)
    ,cid(cid)
    ,name(name)
    ,state(Creating)
{}

ServerChan::~ServerChan() {
    assert(state==Destroy);
    assert(!onClose);
}

/* reached from:
 * 1. connection close
 * 2. DESTROY_CHANNEL
 * 3. local user calls ServerChannelControl::close()
 */
void ServerChan::cleanup()
{
    if(state==ServerChan::Destroy)
        return;
    state = ServerChan::Destroy;

    {
        auto ops(std::move(opByIOID));
        for(auto& op : ops) {
            // removes from conn->opByIOID
            op.second->cleanup();
        }
    }

    auto fn(std::move(onClose));
    if(fn)
        fn("");
}

ServerChannelControl::ServerChannelControl(const std::shared_ptr<ServerConn> &conn, const std::shared_ptr<ServerChan>& channel)
    :server::ChannelControl(channel->name, conn->cred, None)
    ,server(conn->iface->server->internal_self)
    ,chan(channel)
{}

ServerChannelControl::~ServerChannelControl() {}

void ServerChannelControl::onOp(std::function<void(std::unique_ptr<server::ConnectOp>&&)>&& fn)
{
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this, &fn](){
        auto ch = chan.lock();
        if(!ch)
            return;

        ch->onOp = std::move(fn);
    });
}

void ServerChannelControl::onRPC(std::function<void(std::unique_ptr<server::ExecOp>&&, Value&&)>&& fn)
{
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this, &fn](){
        auto ch = chan.lock();
        if(!ch)
            return;

        ch->onRPC = std::move(fn);
    });
}

void ServerChannelControl::onSubscribe(std::function<void(std::unique_ptr<server::MonitorSetupOp>&&)>&& fn)
{
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this, &fn](){
        auto ch = chan.lock();
        if(!ch)
            return;

        ch->onSubscribe = std::move(fn);
    });
}

void ServerChannelControl::onClose(std::function<void(const std::string&)>&& fn)
{
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this, &fn](){
        auto ch = chan.lock();
        if(!ch || ch->state==ServerChan::Destroy)
            return;

        ch->onClose = std::move(fn);
    });
}

void ServerChannelControl::close()
{
    // fail soft if server stopped, or channel/connection already closed
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this](){
        auto ch = chan.lock();
        if(!ch)
            return;
        auto conn = ch->conn.lock();
        if(conn && conn->connection() && ch->state==ServerChan::Active) {
            log_debug_printf(connio, "%s %s Send unsolicited Channel Destroy\n",
                             conn->peerName.c_str(), ch->name.c_str());

            auto tx = bufferevent_get_output(conn->connection());
            EvOutBuf R(conn->sendBE, tx);
            to_wire(R, Header{CMD_DESTROY_CHANNEL, pva_flags::Server, 8});
            to_wire(R, ch->sid);
            to_wire(R, ch->cid);
            conn->statTx += 16u;
            ch->statTx += 16u;
        }

        ch->cleanup();
    });
}

void ServerChannelControl::_updateInfo(const std::shared_ptr<const ReportInfo>& info)
{
    auto serv = server.lock();
    if(!serv)
        return;

    serv->acceptor_loop.call([this, &info](){
        auto ch = chan.lock();
        if(!ch)
            return;
        ch->reportInfo = info;
    });
}

void ServerConn::handle_SEARCH()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t searchID=0;
    uint8_t flags=0;

    from_wire(M, searchID);
    from_wire(M, flags);
    bool mustReply = flags&pva_search_flags::MustReply;
    M.skip(3 + 16 + 2, __FILE__, __LINE__); // unused and replyAddr (we always and only reply to TCP peer)

    bool foundtcp = false;
    bool foundtls = false;
    Size nproto{0};
    from_wire(M, nproto);
    for(size_t i=0; i<nproto.size && !foundtcp && M.good(); i++) {
        std::string proto;
        from_wire(M, proto);
        if(proto=="tcp")
            foundtcp = true;
#ifdef PVXS_ENABLE_OPENSSL
        else if(proto=="tls" && iface->server->tls_context && iface->server->effective.tls_port)
            foundtls = true;
#endif
    }

    uint16_t nchan=0;
    from_wire(M, nchan);

    server::Source::Search op;
    strncpy(op._src, peerName.c_str(), sizeof(op._src)-1);
    op._src[sizeof(op._src)-1] = '\0';
    std::vector<std::pair<uint32_t, std::string>> nameStorage(nchan);
    op._names.resize(nchan);

    for(auto n : range(nchan)) {
        from_wire(M, nameStorage[n].first);
        from_wire(M, nameStorage[n].second);
        op._names[n]._name = nameStorage[n].second.c_str();
    }

    if(!M.good())
        throw std::runtime_error(SB()<<M.file()<<':'<<M.line()<<" TCP Search decode error");

    {
        auto G(iface->server->sourcesLock.lockReader());
        for(const auto& pair : iface->server->sources) {
            try {
                pair.second->onSearch(op);
            }catch(std::exception& e){
                log_exc_printf(serversearch, "Unhandled error in Source::onSearch for '%s' : %s\n",
                           pair.first.second.c_str(), e.what());
            }
        }
    }

    uint16_t nreply = 0;
    for(const auto& name : op._names) {
        if(name._claim)
            nreply++;
    }

    if(!(foundtcp || foundtls))
        return; // no supported protocol, can't reply
    if(nreply==0 && !mustReply)
        return; // no result, and no forced reply

    {
        (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

        EvOutBuf R(sendBE, txBody.get());

        _to_wire<12>(R, iface->server->effective.guid.data(), false, __FILE__, __LINE__);
        to_wire(R, searchID);
        to_wire(R, SockAddr::any(AF_INET));
        if(foundtls) {
            to_wire(R, iface->server->effective.tls_port);
            to_wire(R, "tls"); // prefer TLS

        } else if(foundtcp) {
            to_wire(R, iface->server->effective.tcp_port);
            to_wire(R, "tcp");
        }
        // "found" flag
        to_wire(R, uint8_t(nreply!=0 ? 1 : 0));

        to_wire(R, uint16_t(nreply));
        for(auto i : range(op._names.size())) {
            if(op._names[i]._claim) {
                to_wire(R, uint32_t(nameStorage[i].first));
                log_debug_printf(serversearch, "Search claimed '%s'\n", op._names[i]._name);
            }
        }
    }

    enqueueTxBody(CMD_SEARCH_RESPONSE);
}

void ServerConn::handle_CREATE_CHANNEL()
{
    const auto self = shared_from_this();

    EvInBuf M(peerBE, segBuf.get(), 16);

    auto G(iface->server->sourcesLock.lockReader());

    // one channel create request contains main channel names.
    // each of which will received a separate reply.

    uint16_t count = 0;
    from_wire(M, count);
    for(auto i : range(count)) {
        (void)i;
        uint32_t cid = -1, sid = -1;
        std::string name;
        from_wire(M, cid);
        from_wire(M, name);

        if(!M.good() || name.empty())
            break;

        Status sts{Status::Ok};

        bool claimed = false;

        if(chanBySID.size()==0xffffffff) {
            sts.code = Status::Error;
            sts.msg = "Too many Server channels";
            sts.trace = "pvx:serv:chanidoverflow:";

        } else {
            do {
                sid = nextSID++;
            } while(chanBySID.find(sid)!=chanBySID.end());

            auto chan(std::make_shared<ServerChan>(self, sid, cid, name));
            std::unique_ptr<server::ChannelControl> op(new ServerChannelControl(self, chan));

            for(auto& pair : iface->server->sources) {
                try {
                    pair.second->onCreate(std::move(op));
                    const char* msg = nullptr;

                    if(chan->state!=ServerChan::Creating) {
                        msg = "rejected";

                    } else if(chan->onOp || chan->onRPC || chan->onSubscribe || chan->onClose) {
                        msg = "accepted";
                        claimed = true;

                    } else if(!op) {
                        msg = "discarded";
                    }

                    log_debug_printf(serversearch, "Client %s %s channel to %s through %s\n",
                                     peerName.c_str(),
                                     msg ? msg : "ignored",
                                     name.c_str(), pair.first.second.c_str());

                    if(msg)
                        break;
                }catch(std::exception& e){
                    log_exc_printf(serversearch, "Client %s Unhandled error in onCreate %s,%d %s : %s\n", peerName.c_str(),
                               pair.first.second.c_str(), pair.first.first,
                               typeid(&e).name(), e.what());
                }
            }

            if(claimed && chan->state==ServerChan::Creating) {
                chanBySID[sid] = chan;
                chan->state = ServerChan::Active;

            } else {
                sts.code = Status::Fatal;
                sts.msg = "Refused to create Channel";
                sts.trace = "pvx:serv:refusechan:";
                chan->state = ServerChan::Destroy;

                sid = -1;
            }

            // ServerChannelControl destroyed it not saved by claiming Source
        }


        {
            (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

            EvOutBuf R(sendBE, txBody.get());
            to_wire(R, cid);
            to_wire(R, sid);
            to_wire(R, sts);
            // "spec" calls for uint16_t Access Rights here, but pvAccessCPP don't include this (it's useless anyway)
            if(!R.good()) {
                M.fault(__FILE__, __LINE__);
                log_err_printf(connio, "%s:%d Client %s Encode error in CreateChan\n",
                               M.file(), M.line(), peerName.c_str());
                break;
            }
        }

        enqueueTxBody(CMD_CREATE_CHANNEL);
    }

    if(!M.good()) {
        log_err_printf(connio, "%s:%d Client %s Decode error in CreateChan\n",
                       M.file(), M.line(), peerName.c_str());
        bev.reset();
    }
}

void ServerConn::handle_DESTROY_CHANNEL()
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t sid=-1, cid=-1;

    from_wire(M, sid);
    from_wire(M, cid);
    if(!M.good())
        throw std::runtime_error(SB()<<M.file()<<':'<<M.line()<<" Decode error in DestroyChan");

    auto it = chanBySID.find(sid);
    if(it==chanBySID.end()) {
        log_debug_printf(connsetup, "Client %s DestroyChan non-existent sid=%d cid=%d\n", peerName.c_str(),
                   unsigned(sid), unsigned(cid));
        return;
    }

    auto chan = it->second;
    if(chan->cid!=cid) {
        log_debug_printf(connsetup, "Client %s provides incorrect CID with DestroyChan sid=%d cid=%d!=%d '%s'\n", peerName.c_str(),
                   unsigned(sid), unsigned(chan->cid), unsigned(cid), chan->name.c_str());
    }

    chan->cleanup();

    {
        auto tx = bufferevent_get_output(bev.get());
        EvOutBuf R(sendBE, tx);
        to_wire(R, Header{CMD_DESTROY_CHANNEL, pva_flags::Server, 8});
        to_wire(R, sid);
        to_wire(R, cid);

        if(!R.good())
            bev.reset();

        statTx += 16u;
        // don't bother to increment for channel
    }
}

}} // namespace pvxs::impl
