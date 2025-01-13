/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <epicsAssert.h>
#include <epicsMutex.h>
#include <epicsGuard.h>

#include <deque>

#include <pvxs/log.h>
#include "clientimpl.h"

namespace pvxs {
namespace client {

typedef epicsGuard<epicsMutex> Guard;

DEFINE_LOGGER(monevt, "pvxs.client.monitor");
DEFINE_LOGGER(io, "pvxs.client.io");

namespace {
struct Entry {
    Value val;
    std::exception_ptr exc;
    Entry() = default;
    explicit Entry(Value&& v) :val(std::move(v)) {}
    explicit Entry(const std::exception_ptr& e) :exc(e) {}
};
}

struct SubscriptionImpl final : public OperationBase, public Subscription
{
    // for use in log messages, even after cancel()
    std::string channelName;

    evevent ackTick;

    // const after exec()
    std::weak_ptr<SubscriptionImpl> self; // internal
    std::function<void (Subscription&, const Value&)> onInit;
    std::function<void(Subscription&)> event;
    Value pvRequest;
    bool pipeline = false;
    bool autostart = true;
    bool maskConn = false, maskDiscon = true;
    uint32_t queueSize = 4u, ackAt=0u;

    // only access from loop
    mutable std::weak_ptr<Subscription>     external_internal; // 'self' wrapped to be returned by shared_from_this()

    enum state_t : uint8_t {
        Connecting, // waiting for an active Channel
        Creating,   // waiting for reply to INIT
        Idle,       // waiting for start
        Running,    // waiting for stop
        Done,       // Finished or error
    } state = Connecting;

    mutable epicsMutex lock;

    // guarded by lock

    std::deque<Entry> queue;
    uint32_t window =0u; // flow control window.  number of updates server may send to us
    uint32_t unack =0u;  // updates pop()'d, but not ack'd
    size_t nSrvSquash =0u;
    size_t nCliSquash =0u;
    size_t queueMax =0u;
    // user code has seen pop()==nullptr
    bool needNotify = true;
    bool ackPending = false; // ackTick scheduled

    INST_COUNTER(SubscriptionImpl);

    explicit SubscriptionImpl(const evbase& loop)
        :OperationBase (Operation::Monitor, loop)
        ,ackTick(__FILE__, __LINE__,
                 event_new(loop.base, -1, EV_TIMEOUT, &tickAckS, this))
    {}
    virtual ~SubscriptionImpl() {
        if(loop.assertInRunningLoop())
            _cancel(true);
    }

    virtual const std::string& _name() override final {
        return channelName;
    }

    // caller must hold lock
    bool wantToNotify()
    {
        log_info_printf(monevt, "Server %s channel '%s' monitor %snotify\n",
                        chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                        chan->name.c_str(), needNotify ? "" : "skip ");

        bool doit = needNotify;
        needNotify = false;
        return doit;
    }

    // caller must not hold lock
    // call must be from worker
    void doNotify()
    {
        if(event) {
            try {
                event(*this);
            }catch(std::exception& e){
                log_exc_printf(io, "Unhandled user exception in Monitor %s %s : %s\n",
                                __func__, typeid (e).name(), e.what());
            }
        }
    }

    virtual void pause(bool p) override final
    {
        loop.call([this, p](){
            log_info_printf(io, "Server %s channel %s monitor %s\n",
                            chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                            chan->name.c_str(),
                            p ? "pause" : "resume");

            if((state==Idle && !p) || (state==Running && p)) {
                auto& conn = chan->conn;

                {
                    uint8_t subcmd = p ? 0x04 : 0x44; // STOP | START

                    (void)evbuffer_drain(conn->txBody.get(), evbuffer_get_length(conn->txBody.get()));

                    EvOutBuf R(conn->sendBE, conn->txBody.get());

                    to_wire(R, chan->sid);
                    to_wire(R, ioid);
                    to_wire(R, subcmd);
                }
                chan->statTx += conn->enqueueTxBody(CMD_MONITOR);

                state = p ? Idle : Running;
            }
        });
    }

    virtual void stats(SubscriptionStat& ret, bool reset) override final {
        Guard G(lock);
        ret.limitQueue = queueSize;
        ret.maxQueue = queueMax;
        ret.nSrvSquash = nSrvSquash;
        ret.nCliSquash = nCliSquash;
        ret.nQueue = queue.size();
        if(reset) {
            nSrvSquash = nCliSquash = queueMax = 0u;
        }
    }

    void _pop(Value& ret, bool canthrow)
    {
        {
            if(!queue.empty()) {

                if(!canthrow && queue.front().exc)
                    return;

                auto ent(std::move(queue.front()));
                queue.pop_front();

                if(pipeline) {
                    timeval tick{}; // immediate ACK

                    unack++;

                    if(!ackPending && unack>=ackAt) {
                        if(event_add(ackTick.get(), &tick)) {
                            log_err_printf(io, "Monitor '%s' unable to schedule ack\n", channelName.c_str());
                        } else {
                            log_debug_printf(io, "Monitor '%s' sched ack %u/%u\n",
                                             channelName.c_str(), unsigned(unack), unsigned(ackAt));
                            ackPending = true;
                        }
                    }
                }
                log_printf(monevt, ent.exc || ent.val ? Level::Info : Level::Err,
                           "channel '%s' monitor pop() %s %u,%u\n",
                           channelName.c_str(),
                           ent.exc ? "exception" : ent.val ? "data" : "null!",
                           unsigned(window), unsigned(unack));

                if(ent.exc)
                    std::rethrow_exception(ent.exc);
                else
                    ret = std::move(ent.val);

            } else {
                needNotify = true;

                log_info_printf(monevt, "channel '%s' monitor pop() empty\n",
                                channelName.c_str());
            }
        }
    }

    virtual Value pop() override final
    {
        Value ret;
        {
            Guard G(lock);
            _pop(ret, true);
        }
        return ret;
    }

    virtual bool doPop(std::vector<Value>& out, size_t limit) override final
    {
        out.clear();

        if(!limit) {
            limit = queueSize; // alloc for worst case
        }

        out.reserve(limit);

        Guard G(lock);

        while(out.size() < limit) {
            Value temp;
            _pop(temp, out.empty()); // only throw if out is empty
            if(!temp)
                break;

            out.emplace_back(std::move(temp));
        }

        return !needNotify;
    }

    virtual std::shared_ptr<Subscription> shared_from_this() const override final {
        // on worker?
        std::shared_ptr<Subscription> ret;
        loop.call([this, &ret](){
            // really on worker

            // try to reuse already wrapped
            ret = external_internal.lock();
            if(!ret) {
                // nope, need to build a fresh one

                // we want to return 'self' to user code, but need to ensure it is
                // disposed of from our worker thread.
                std::shared_ptr<SubscriptionImpl> strong(self);

                ret.reset(strong.get(), [strong](Subscription*) mutable {
                    // on worker?
                    auto junk(std::move(strong));
                    // need to do cleanup on worker if running
                    auto loop(junk->loop);
                    loop.tryCall(std::bind([](std::shared_ptr<SubscriptionImpl>& junk) noexcept {
                         // really on worker
                         // cleanup here when worker is running
                         junk.reset();
                     }, std::move(junk)));
                    // or cleanup here when worker is stopped, and lambda is destroyed
                });
                // hack: external_internal is 'mutable' so that shared_from_this() can appear to be const
                external_internal = ret;
            }
        });
        return ret;
    }

    virtual void _onEvent(std::function<void(Subscription&)>&& fn) override final {
        decltype (event) junk;
        loop.call([this, &junk, &fn]() {
            junk = std::move(event);
            this->event = std::move(fn);
        });
    }

    virtual bool cancel() override final {
        decltype (event) junk;
        bool ret = false;
        (void)loop.tryCall([this, &junk, &ret](){
            ret = _cancel(false);
            junk = std::move(event);
            // leave opByIOID for GC
        });
        return ret;
    }

    bool _cancel(bool implicit) {
        if(implicit && state!=Done) {
            log_info_printf(io, "Server %s channel %s monitor implied cancel\n",
                            chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                            chan->name.c_str());
        }
        log_info_printf(io, "Server %s channel %s monitor cancel\n",
                        chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                        chan->name.c_str());

        if(state==Idle || state==Running) {
            chan->conn->sendDestroyRequest(chan->sid, ioid);

            // This opens up a race with an in-flight reply.
            chan->conn->opByIOID.erase(ioid);
            chan->opByIOID.erase(ioid);

            if(pipeline)
                (void)event_del(ackTick.get());
        }
        bool ret = state!=Done;
        state = Done;
        return ret;
    }

    // not actually visible through Subscription.
    // an artifact of using OperationBase for convenience
    void _reExecGet(std::function<void(client::Result&&)>&& resultcb) override final {}
    void _reExecPut(const Value& arg, std::function<void(client::Result&&)>&& resultcb) override final {}

    virtual void createOp() override final
    {
        if(state!=Connecting) {
            return;
        }

        auto& conn = chan->conn;

        {
            uint8_t subcmd = 0x08; // INIT
            if(pipeline)
                subcmd |= 0x80;

            (void)evbuffer_drain(conn->txBody.get(), evbuffer_get_length(conn->txBody.get()));

            EvOutBuf R(conn->sendBE, conn->txBody.get());

            to_wire(R, chan->sid);
            to_wire(R, ioid);
            to_wire(R, subcmd);
            to_wire(R, Value::Helper::desc(pvRequest));
            to_wire_full(R, pvRequest);
            if(pipeline)
                to_wire(R, queueSize);
        }
        chan->statTx += conn->enqueueTxBody(CMD_MONITOR);

        log_debug_printf(io, "Server %s channel '%s' monitor INIT%s q=%u a=%u\n",
                         conn->peerName.c_str(), chan->name.c_str(), pipeline?" pipeline":"",
                         unsigned(queueSize), unsigned(ackAt));

        state = Creating;

        bool notify = false;
        if(!maskConn || pipeline) {
            Guard G(lock);

            if(!maskConn) {
                notify = queue.empty() && wantToNotify();

                queue.emplace_back(std::make_exception_ptr(Connected(conn->peerName,
                                                                     conn->connTime,
                                                                     conn->cred)));

                log_debug_printf(io, "Server %s channel %s monitor PUSH Connected\n",
                                 chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                                 chan->name.c_str());
            }
            if(pipeline)
                window = queueSize;
        }

        if(notify)
            doNotify();
    }

    virtual void disconnected(const std::shared_ptr<OperationBase> &self) override final
    {
        log_debug_printf(io, "Server %s channel %s monitor disconnected in %d\n",
                        chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                        chan->name.c_str(),
                        state);

        switch (state) {
        case Connecting:
        case Done:
            // noop
            break;
        case Creating:
        case Idle:
        case Running:
            // return to pending

            bool notify = false;
            if(!maskDiscon) {
                Guard G(lock);
                notify = queue.empty() && wantToNotify();

                queue.emplace_back(std::make_exception_ptr(Disconnect()));

                log_debug_printf(io, "Server %s channel %s monitor PUSH Disconnect\n",
                                 chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                                 chan->name.c_str());
            }

            chan->pending.push_back(self);
            state = Connecting;

            if(notify)
                doNotify();

            break;
        }
    }

    void tickAck()
    {
        uint32_t num2ack = 0;
        {
            Guard G(lock);

            ackPending = false;

            if(((state==Idle) || (state==Running)) && pipeline && unack) {
                num2ack = unack;
                window += unack;
                unack = 0u;

                log_debug_printf(io, "Server %s channel %s monitor ACK %u\n",
                                chan->conn ? chan->conn->peerName.c_str() : "<disconnected>",
                                chan->name.c_str(), unsigned(num2ack));
            }

        }

        if(num2ack) {

            auto& conn = chan->conn;
            {
                (void)evbuffer_drain(conn->txBody.get(), evbuffer_get_length(conn->txBody.get()));

                EvOutBuf R(conn->sendBE, conn->txBody.get());

                to_wire(R, chan->sid);
                to_wire(R, ioid);
                to_wire(R, uint8_t(0x80));
                to_wire(R, uint32_t(num2ack));
            }
            chan->statTx += conn->enqueueTxBody(CMD_MONITOR);
        }
    }
    static
    void tickAckS(evutil_socket_t fd, short evt, void *raw)
    {
        try {
            static_cast<SubscriptionImpl*>(raw)->tickAck();
        }catch(std::exception& e) {
            log_exc_printf(io, "Unhandled exception in %s %s : %s\n",
                           __func__, typeid (e).name(), e.what());
        }
    }
};
DEFINE_INST_COUNTER(SubscriptionImpl);

void Connection::handle_MONITOR()
{
    auto rxlen = 8u + evbuffer_get_length(segBuf.get());
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t ioid=0;
    uint8_t subcmd=0;
    Status sts{};
    Value data; // hold prototype (INIT) or reply data

    from_wire(M, ioid);
    from_wire(M, subcmd);
    bool init = subcmd&0x08;
    bool final = subcmd&0x10;

    if(init || final)
        from_wire(M, sts);
    if(init && sts.isSuccess())
        from_wire_type(M, rxRegistry, data);

    RequestInfo* info=nullptr;
    bool servSquash = false;
    if(M.good()) {
        auto it = opByIOID.find(ioid);
        if(it!=opByIOID.end()) {
            info = &it->second;

        } else {
            if(!init) {
                rxRegistryDirty = true;
            }

            log_debug_printf(io,  "Server %s uses non-existent IOID %u.  Ignoring...\n",
                       peerName.c_str(), unsigned(ioid));
            return;
        }

        if(!sts.isSuccess()) {

        } else if(init) {
            info->prototype = std::move(data);
            // initialize info->fl later, with access to queueSize

        } else if(!final || !M.empty()) {

            // Take from free-list of pre-allocated Value
            Value raw;
            {
                Guard G(info->fl->lock);

                if(!info->fl->unused.empty()) {
                    raw = std::move(info->fl->unused.back());
                    info->fl->unused.pop_back();

                } else {
                    raw = info->prototype.cloneEmpty();
                }
            }
            // Wrap Value for automatic return to our free-list
            {
                std::weak_ptr<RequestFL> wfl(info->fl);
                auto desc(Value::Helper::desc(raw));
                auto store(Value::Helper::store_ptr(raw));

                Value::Helper::store(data).reset(
                            store,
                            // ugly bind() to capture by move instead of copy to avoid extra ref-counts
                            std::bind(
                            [](FieldStorage*, Value& data, std::weak_ptr<RequestFL>& wfl) mutable {
                                // maybe on worker or user thread
                                auto real(std::move(data));
                                if(auto fl = wfl.lock()) {
                                    Guard G(fl->lock);
                                    if(fl->unused.size() < fl->limit) {
                                        real.clear();
                                        fl->unused.emplace_back(std::move(real));
                                    }
                                }

                }, std::placeholders::_1, std::move(raw), std::move(wfl))
                            );

                Value::Helper::set_desc(data, desc);
            }
            from_wire_valid(M, rxRegistry, data);

            cache_sync(info->prototype, data);

            BitMask overrun;
            from_wire(M, overrun);
            for(auto i : range(overrun.wsize())) {
                (void)i;
                if(overrun.word(i)) {
                    // this update Value is the result of combining
                    // two or more Values on the server side.
                    servSquash = true;
                    break;
                }
            }
        }
    }

    // validate received message against operation state

    std::shared_ptr<OperationBase> op;
    SubscriptionImpl* mon = nullptr;
    if(M.good() && info) {
        op = info->handle.lock();
        if(!op) {
            // assume op has already sent CMD_DESTROY_REQUEST
            log_debug_printf(io, "Server %s ignoring stale cmd%02x ioid %u\n",
                             peerName.c_str(), CMD_MONITOR, unsigned(ioid));
            return;
        }

        if(uint8_t(op->op)!=CMD_MONITOR) {
            // peer mixes up IOID and operation type
            M.fault(__FILE__, __LINE__);

        } else {
            mon = static_cast<SubscriptionImpl*>(op.get());

            // check that subcmd is as expected based on operation state
            if((mon->state==SubscriptionImpl::Creating) && init) {

            } else if((mon->state==SubscriptionImpl::Idle) && !init) {

            } else if((mon->state==SubscriptionImpl::Running) && !init) {

            } else {
                M.fault(__FILE__, __LINE__);
            }
        }
    }

    if(!M.good() || !mon) {
        log_crit_printf(io, "%s:%d Server %s sends invalid MONITOR.  Disconnecting...\n",
                        M.file(), M.line(), peerName.c_str());
        bev.reset();
        return;
    }

    mon->chan->statRx += rxlen;

    Entry update;

    if(!sts.isSuccess()) {
        update.exc = std::make_exception_ptr(RemoteError(sts.msg));
        mon->state = SubscriptionImpl::Done;

    } else if(mon->state==SubscriptionImpl::Creating) {
        log_debug_printf(io, "Server %s channel %s monitor Created\n",
                        peerName.c_str(),
                        mon->chan->name.c_str());

        mon->state = SubscriptionImpl::Idle;

        try {
            if(mon->onInit)
                mon->onInit(*mon, info->prototype);
        }catch(std::exception& e){
            mon->state = SubscriptionImpl::Done;
            update.exc = std::current_exception();
            log_debug_printf(io, "Server %s channel %s monitor Create error: %s\n",
                            peerName.c_str(),
                            mon->chan->name.c_str(), e.what());
        }

        if(mon->autostart && mon->state == SubscriptionImpl::Idle)
            mon->resume();

    } else if(data) { // Idle or Running
        update.val = std::move(data);

    } else {
        // NULL update.  can this happen?
        log_debug_printf(io, "Server %s channel %s monitor RX NULL\n",
                        peerName.c_str(),
                        mon->chan->name.c_str());
    }

    bool notify = false;
    {
        Guard G(mon->lock);

        if(init && !sts.isSuccess()) {
            log_debug_printf(io, "Server %s channel %s monitor PUSH init error\n",
                            peerName.c_str(),
                            mon->chan->name.c_str());

            mon->queue.emplace_back(std::move(update));
            notify = true;

        } else if(init) {
            /* Allow enough for user to hold/process one full queue while
             * accumulate another.
             */
            info->fl = std::make_shared<RequestFL>(2u*mon->queueSize);

        } else {

            if(mon->pipeline) {
                if(mon->window) {
                    mon->window--;

                    if(!mon->window)
                        log_debug_printf(io, "Server %s channel '%s' MONITOR zero window w/ %u\n",
                                        peerName.c_str(), mon->chan->name.c_str(), unsigned(mon->unack));

                } else if(!final) {
                    log_err_printf(io, "Server %s channel '%s' MONITOR exceeds window size\n",
                                    peerName.c_str(), mon->chan->name.c_str());
                }
            }

            notify = mon->queue.empty();

            assert(mon->queueSize >= 1u);
            if(update.val && mon->queue.size() >= mon->queueSize && mon->queue.back().val && !mon->pipeline) {
                log_debug_printf(io, "Server %s channel %s monitor Squash\n",
                                 peerName.c_str(),
                                 mon->chan->name.c_str());

                mon->queue.back().val.assign(update.val);
                mon->nCliSquash++;

            } else if(update.exc || update.val) {
                log_debug_printf(io, "Server %s channel %s monitor PUSH\n",
                                peerName.c_str(),
                                mon->chan->name.c_str());

                mon->queue.emplace_back(std::move(update));

            }

            if(final) {
                log_debug_printf(io, "Server %s channel %s monitor FINISH\n",
                                peerName.c_str(),
                                mon->chan->name.c_str());

                mon->queue.emplace_back(std::make_exception_ptr(Finished()));
            }

            if(mon->queue.empty()) {
                log_err_printf(io, "Server %s channel '%s' monitor empty update!\n",
                               peerName.c_str(), mon->chan->name.c_str());
                notify = false;
            }

            if(mon->queueMax < mon->queue.size())
                mon->queueMax = mon->queue.size();
        }

        if(notify)
            notify = mon->wantToNotify();
        if(servSquash)
            mon->nSrvSquash++;
    } // release mon->lock

    if(mon->state==SubscriptionImpl::Done || final) {
        mon->state=SubscriptionImpl::Done;

        opByIOID.erase(ioid);
        mon->chan->opByIOID.erase(ioid);

        if(mon->pipeline)
            (void)event_del(mon->ackTick.get());

        if(!final)
            sendDestroyRequest(mon->chan->sid, ioid);
    }

    if(notify)
        mon->doNotify();
}


std::shared_ptr<Subscription> MonitorBuilder::exec()
{
    if(!ctx)
        throw std::logic_error("NULL Builder");

    auto context(ctx->impl->shared_from_this());

    auto op(std::make_shared<SubscriptionImpl>(context->tcp_loop));
    op->self = op;
    op->channelName = std::move(_name);
    op->event = std::move(_event);
    op->onInit = std::move(_onInit);
    op->pvRequest = _buildReq();
    op->maskConn = _maskConn;
    op->maskDiscon = _maskDisconn;
    op->autostart = _autoexec;

    auto options = op->pvRequest["record._options"];

    options["queueSize"].as<uint32_t>([&op](uint32_t Q) {
        if(Q>1)
            op->queueSize = Q;
    });

    (void)options["pipeline"].as(op->pipeline);

    auto ackAny = options["ackAny"];

    if(ackAny.type()==TypeCode::String) {
        auto sval = ackAny.as<std::string>();
        if(sval.size()>1 && sval.back()=='%') {
            try {
                auto percent = parseTo<double>(sval.substr(0, sval.size()-1u));
                if(percent>0.0 && percent<=100.0) {
                    op->ackAt = uint32_t(percent * op->queueSize);
                } else {
                    throw std::invalid_argument("not in range (0%, 100%]");
                }
            }catch(std::exception&){
                log_warn_printf(monevt, "Error parsing as percent ackAny: \"%s\"\n", sval.c_str());
            }
        }

    }

    if(op->ackAt==0u){
        uint32_t count=0u;

        if(ackAny.as(count)) {
            op->ackAt = count;
        }
    }

    if(op->ackAt==0u){
        op->ackAt = op->queueSize/2u;
    }

    op->ackAt = std::max(1u, std::min(op->ackAt, op->queueSize));

    auto syncCancel(_syncCancel);
    std::shared_ptr<SubscriptionImpl> external(op.get(), [op, syncCancel](SubscriptionImpl*) mutable {
        // from user thread
        auto temp(std::move(op));
        auto loop(temp->loop);
        // std::bind for lack of c++14 generalized capture
        // to move internal ref to worker for dtor
        loop.tryInvoke(syncCancel, std::bind([](std::shared_ptr<SubscriptionImpl>& op) {
                           // on worker

                           // ordering of dispatch()/call() ensures creation before destruction
                           if(op->chan)
                               op->_cancel(true);
                       }, std::move(temp)));
    });

    auto server(std::move(_server));
    context->tcp_loop.dispatch([op, context, server]() {
        // on worker

        try {
            op->chan = Channel::build(context, op->channelName, server);

            op->chan->pending.push_back(op);
            op->chan->createOperations();
        }catch(...){
            // nothing else has happened, so the queue will be empty
            assert(op->queue.empty());
            op->queue.emplace_back();
            op->queue.back().exc = std::current_exception();
            op->doNotify();
        }
    });

    return external;
}

} // namespace client
} // namespace pvxs
