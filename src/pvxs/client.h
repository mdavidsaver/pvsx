/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#ifndef PVXS_CLIENT_H
#define PVXS_CLIENT_H

#include <stdexcept>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <iosfwd>
#include <typeinfo>

#include <epicsTime.h>
#include <epicsEndian.h>

#include <pvxs/version.h>
#include <pvxs/data.h>
#include <pvxs/netcommon.h>
#include <pvxs/util.h>

namespace pvxs {
namespace client {

class Context;
struct Config;

//! Operation failed because of connection to server was lost
struct PVXS_API Disconnect : public std::runtime_error
{
    Disconnect();
    virtual ~Disconnect();

    //! When loss of connection was noticed (when timeout expires).
    const epicsTime time;
};

//! Error condition signaled by server
struct PVXS_API RemoteError : public std::runtime_error
{
    RemoteError(const std::string& msg);
    virtual ~RemoteError();
};

//! For monitor only.  Subscription has completed normally
//! and no more events will ever be received.
struct PVXS_API Finished : public Disconnect
{
    Finished() = default;
    virtual ~Finished();
};

//! For monitor only.  Subscription has (re)connected.
struct PVXS_API Connected : public std::runtime_error
{
    Connected(const std::string& peerName);
    virtual ~Connected();

    const std::string peerName;
    const epicsTime time;
};

struct PVXS_API Interrupted : public std::runtime_error
{
    Interrupted();
    virtual ~Interrupted();
};

struct PVXS_API Timeout : public std::runtime_error
{
    Timeout();
    virtual ~Timeout();
};

//! Holder for a Value or an exception
class Result {
    Value _result;
    std::exception_ptr _error;
    std::string _peerName;
public:
    Result() = default;
    Result(Value&& val, const std::string& peerName) :_result(std::move(val)), _peerName(peerName) {}
    explicit Result(const std::exception_ptr& err) :_error(err) {}

    //! Access to the Value, or rethrow the exception
    Value& operator()() {
        if(_error)
            std::rethrow_exception(_error);
        return _result;
    }

    const std::string peerName() const { return  _peerName; }

    bool error() const { return !!_error; }
    explicit operator bool() const { return _result || _error; }
};

//! Handle for in-progress operation
struct PVXS_API Operation {
    //! Operation type
    const enum operation_t {
        Info    = 17, // CMD_GET_FIELD
        Get     = 10, // CMD_GET
        Put     = 11, // CMD_PUT
        RPC     = 20, // CMD_RPC
        Monitor = 13, // CMD_MONITOR
        Discover = 3, // CMD_SEARCH
    } op;

    explicit constexpr Operation(operation_t op) :op(op) {}
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    virtual ~Operation() =0;

    //! PV name
    virtual const std::string& name() =0;

    //! Explicitly cancel a pending operation.
    //! Blocks until an in-progress callback has completed.
    //! @returns true if the operation was canceled, or false if already complete.
    virtual bool cancel() =0;

    /** @brief Block until Operation completion
     *
     * As an alternative to a .result() callback, wait for operation completion,
     * timeout, or interruption (via. interrupt() ).
     *
     * @param timeout Time to wait prior to throwing TimeoutError.  cf. epicsEvent::wait(double)
     * @return result Value.  Always empty/invalid for put()
     * @throws Timeout Timeout exceeded
     * @throws Interrupted interrupt() called
     */
    virtual Value wait(double timeout) =0;

    //! wait(double) without a timeout
    Value wait() {
        return wait(99999999.0);
    }

    //! Queue an interruption of a wait() or wait(double) call.
    virtual void interrupt() =0;

protected:
    virtual void _reExecGet(std::function<void(client::Result&&)>&& resultcb) =0;
    virtual void _reExecPut(const Value& arg, std::function<void(client::Result&&)>&& resultcb) =0;
public:
#ifdef PVXS_EXPERT_API_ENABLED
    // usable when Builder::autoExec(false)
    // For GET/PUT, (re)issue request for current value
    inline void reExecGet(std::function<void(client::Result&&)>&& resultcb) { this->_reExecGet(std::move(resultcb)); }
    // For PUT (re)issue request to set current value
    inline void reExecPut(const Value& arg, std::function<void(client::Result&&)>&& resultcb) { this->_reExecPut(arg, std::move(resultcb)); }
#endif
};

//! Information about the state of a Subscription
struct SubscriptionStat {
    //! Number of events in the queue
    size_t nQueue=0;
    //! Number of Value updates where the server reported at least
    //! one update dropped/squashed.
    size_t nSrvSquash=0;
    //! Number of Value updates dropped/squashed due to client queue overflow
    size_t nCliSquash=0;
    //! Max queue size so far
    size_t maxQueue=0;
    //! Limit on queue size
    size_t limitQueue=0;
};

//! Handle for monitor subscription
struct PVXS_API Subscription {
    Subscription() = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    virtual ~Subscription() =0;

protected:
    virtual const std::string& _name() =0;
public:
    //! PV name
    inline const std::string& name() { return _name(); }

    //! Explicitly cancel a active subscription.
    //! Blocks until any in-progress callback has completed.
    virtual bool cancel() =0;

    //! Ask a server to stop (true) or re-start (false), sending updates to this Subscription
    virtual void pause(bool p=true) =0;
    //! Shorthand for @code pause(false) @endcode
    inline void resume() { pause(false); }

    /** De-queue update from subscription event queue.
     *
     * If the queue is empty, return an empty/invalid Value (Value::valid()==false).
     * A data update is returned as a Value.
     * An error or special event is thrown.
     *
     * @returns A valid Value until the queue is empty
     * @throws Connected (depending on MonitorBuilder::maskConnected())
     * @throws Disconnect (depending on MonitorBuilder::maskDisconnect())
     * @throws Finished  (depending on MonitorBuilder::maskDisconnect())
     * @throws RemoteError For server signaled errors
     * @throws std::exception For client side failures.
     *
     * @code
     * std::shared_ptr<Subscription> sub(...);
     * try {
     *     while(auto update = sub.pop()) {
     *         // have data update
     *         ...
     *     }
     *     // queue empty
     * } catch(Connected& con) {   // if MonitorBuilder::maskConnected(false)
     * } catch(Finished& con) {    // if MonitorBuilder::maskDisconnected(false)
     * } catch(Disconnect& con) {  // if MonitorBuilder::maskDisconnected(false)
     * } catch(RemoteError& con) { // error message from server
     * } catch(std::exception& con) { // client side error
     * }
     * @endcode
     */
    virtual Value pop() =0;

protected:
    virtual bool doPop(std::vector<Value>& out, size_t limit=0u) =0;
public:

#ifdef PVXS_EXPERT_API_ENABLED
    /** De-queue a batch of updates from subscription event queue.
     *
     * @param out Updated with any Values dequeued.  Will always be clear()d
     * @param limit When non-zero, an upper limit on the number of Values which will be dequeued.
     * @return true if the queue was not emptied, and pop() should be called again.
     *         false if the queue was emptied, and a further onEvent() callback may be awaited.
     * @throws the same exceptions as non-batch pop()
     *
     * @since 1.1.0 Added
     */
    inline bool pop(std::vector<Value>& out, size_t limit=0u)
    { return doPop(out, limit); }
#endif

    //! Poll statistics
    //! @since 1.1.0
    virtual void stats(SubscriptionStat&, bool reset = false) =0;

protected:
    virtual void _onEvent(std::function<void(Subscription&)>&&) =0;
public:
#ifdef PVXS_EXPERT_API_ENABLED
    // replace handler stored with MonitorBuilder::event()
    inline void onEvent(std::function<void(Subscription&)>&& fn) { this->_onEvent(std::move(fn)); }
#endif

    //! Return strong internal reference which will not prevent
    //! implicit cancellation when the last reference returned
    //! by exec() is released.
    //! @since 0.2.0
    virtual std::shared_ptr<Subscription> shared_from_this() const =0;
};

//! Handle for entry in Channel cache
struct PVXS_API Connect {
    virtual ~Connect() =0;

    //! Name passed to Context::connect()
    virtual const std::string& name() const =0;
    //! Poll (momentary) connection status
    virtual bool connected() const =0;
};

class GetBuilder;
class PutBuilder;
class RPCBuilder;
class MonitorBuilder;
class RequestBuilder;
class ConnectBuilder;
struct Discovered;
class DiscoverBuilder;

/** An independent PVA protocol client instance
 *
 *  Typically created with Config::build()
 *
 *  @code
 *  Context ctxt(Config::from_env().build());
 *  @endcode
 */
class PVXS_API Context {
public:
    struct Pvt;

    //! An empty/dummy Context
    constexpr Context() = default;
    //! Create/allocate a new client with the provided config.
    //! Config::build() is a convenient shorthand.
    explicit Context(const Config &);
    ~Context();

    /** Create new client context based on configuration from $EPICS_PVA* environment variables.
     *
     * Shorthand for @code Config::fromEnv().build() @endcode.
     * @since 0.2.1
     */
    static
    Context fromEnv();

    //! effective config of running client
    const Config& config() const;

    /** Force close the client.
     *
     * ~Context() will close() automatically.  So an explicit call is optional.
     *
     * Aborts/interrupts all in progress network operations.
     * Blocks until any in-progress callbacks have completed.
     *
     * @since 1.1.0
     */
    void close();

    /** Request the present value of a PV
     *
     * Simple blocking
     *
     * @code
     * Context ctxt(...);
     * auto result = ctxt.get("pv:name")
     *                   .exec()
     *                   ->wait();
     * @endcode
     *
     * With completion callback
     *
     * @code
     * Context ctxt(...);
     * auto op = ctxt.get("pv:name")
     *               .result([](Result&& prototype){
     *                  std::cout<<prototype();
     *               })
     *               .exec();
     * // store op until completion
     * @endcode
     * See GetBuilder and <a href="#get-info">Get/Info</a> for details.
     */
    inline
    GetBuilder get(const std::string& pvname);

    /** Request type information from PV.
     *  Results in a Value with no marked fields.
     *
     * Simple blocking
     *
     * @code
     * Context ctxt(...);
     * auto result = ctxt.info("pv:name")
     *                   .exec()
     *                   ->wait();
     * @endcode
     *
     * With completion callback
     *
     * @code
     * Context ctxt(...);
     * auto op = ctxt.info("pv:name")
     *               .result([](Result&& prototype){
     *                  std::cout<<prototype();
     *               })
     *               .exec();
     * // store op until completion
     * @endcode
     *
     * See GetBuilder and <a href="#get-info">Get/Info</a> for details.
     */
    inline
    GetBuilder info(const std::string& pvname);

    /** Request change/update of PV.
     *
     * Assign certain values to certain fields and block for completion.
     *
     * @code
     * Context ctxt(...);
     * auto result = ctxt.put("pv:name")
     *                   .set("value", 42)
     *                   .exec()
     *                   ->wait();
     * @endcode
     *
     * Alternately, and more generally, using a .build() callback
     * and use .result() callback for completion notification.
     *
     * @code
     * Context ctxt(...);
     * auto op = ctxt.put("pv:name")
     *               .build([](Value&& prototype) -> Value {
     *                   auto putval = prototype.cloneEmpty();
     *                   putval["value"] = 42;
     *                   return putval;
     *               })
     *               .result([](Result&& prototype){
     *                  try {
     *                      // always returns empty Value on success
     *                      prototype();
     *                      std::cout<<"Success";
     *                  }catch(std::exception& e){
     *                      std::cout<<"Error: "<<e.what();
     *                  }
     *               })
     *               .exec();
     * // store op until completion
     * @endcode
     *
     * See PutBuilder and <a href="#put">Put</a> for details.
     */
    inline
    PutBuilder put(const std::string& pvname);

    inline
    RPCBuilder rpc(const std::string& pvname);

    /** Execute "stateless" remote procedure call operation.
     *
     * Simple blocking
     *
     * @code
     * Value arg = ...;
     * Context ctxt(...);
     * auto result = ctxt.rpc("pv:name", arg)
     *                   .arg("blah", 5)
     *                   .arg("other", "example")
     *                   .exec()
     *                   ->wait();
     * @endcode
     *
     * With completion callback
     *
     * @code
     * Value arg = ...;
     * Context ctxt(...);
     * auto op = ctxt.rpc("pv:name", arg)
     *               .result([](Result&& prototype){
     *                  std::cout<<prototype();
     *               })
     *               .exec();
     * // store op until completion
     * @endcode
     *
     * See RPCBuilder and <a href="#rpc">RPC</a> for details.
     */
    inline
    RPCBuilder rpc(const std::string& pvname, const Value& arg);

    /** Create a new subscription for changes to a PV.
     *
     * @code
     * MPMCFIFO<std::shared_ptr<Subscription>> workqueue(42u);
     *
     * auto sub = ctxt.monitor("pv:name")
     *                .event([&workqueue](Subscription& sub) {
     *                    // Subscription queue becomes not empty.
     *                    // Avoid I/O on PVXS worker thread,
     *                    // delegate to application thread
     *                    workqueue.push(sub.shared_from_this());
     *                })
     *                .exec();
     *
     * while(auto sub = workqueue.pop()) { // could workqueue.push(nullptr) to break
     *     try {
     *         Value update = sub.pop();
     *         if(!update)
     *             continue; // Subscription queue empty, wait for another event callback
     *         std::cout<<update<<"\n";
     *     } catch(std::exception& e) {
     *         // may be Connected(), Disconnect(), Finished(), or RemoteError()
     *         std::cerr<<"Error "<<e.what()<<"\n";
     *     }
     *     // queue not empty, reschedule
     *     workqueue.push(sub);
     * }
     * // store op until completion
     * @endcode
     *
     * See MonitorBuilder and <a href="#monitor">Monitor</a> for details.
     */
    inline
    MonitorBuilder monitor(const std::string& pvname);

    /** Manually add, and maintain, an entry in the Channel cache.
     *
     * This optional method may be used when it is known that a given PV
     * will be needed in future.
     * ConnectBuilder::onConnect() and ConnectBuilder::onDisconnect()
     * may be used to get asynchronous notification, or
     * the returned Connect object may be used to poll Channel (dis)connect state.
     *
     * @since 0.2.0
     */
    inline
    ConnectBuilder connect(const std::string& pvname);

    /** Compose a pvRequest independently of a network operation.
     *
     * This is not a network operation.
     *
     * Use of request() is optional.  pvRequests can be composed
     * with individual network operation Builders.
     *
     * @code
     * Value pvReq = Context::request()
     *                      .pvRequest("field(value)field(blah)")
     *                      .record("pipeline", true)
     *                      .build();
     * @endcode
     */
    static inline
    RequestBuilder request();

    /** Discover the presence or absence of Servers.
     *
     * Combines information from periodic Server Beacon messages, and optionally
     * Discover pings, to provide notice when PVA servers appear or disappear
     * from attached networks.
     *
     * Note that a discover() Operation will never complete with a Value,
     * and so can only end with a timeout or cancellation.
     *
     * @code
     * Context ctxt(...);
     * auto op = ctxt.discover([](const Discovered& evt) {
     *                  std::cout<<evt<<std::endl;
     *              })
     *              .pingAll(false) // implied default
     *              .exec();
     * op->wait(10.0); // wait 10 seconds, will always timeout.
     * @endcode
     *
     * @since 0.3.0
     */
    inline
    DiscoverBuilder discover(std::function<void(const Discovered &)> && fn);

    /** Request prompt search of any disconnected channels.
     *
     * This method is recommended for use when executing a batch of operations.
     *
     * @code
     * Context ctxt = ...;
     * std::vector<std::string> pvnames = ...;
     * std::vector<Operation> ops(pvnames.size());
     *
     * // Initiate all operations
     * for(size_t i=0; i<pvname.size(); i++)
     *     ops[i] = ctxt.get(pvnames[i]).exec();
     *
     * ctxt.hurryUp(); // indicate end of batch
     *
     * for(size_t i=0; i<pvname.size(); i++)
     *     ... = ops[i].wait(); // wait for results
     * @endcode
     *
     * Optional.  Equivalent to detection of a new server.
     * This method has no effect if called more often than once per 30 seconds.
     */
    void hurryUp();

#ifdef PVXS_EXPERT_API_ENABLED
    //! Actions of cacheClear()
    //! @since 0.2.0
    enum cacheAction {
        Clean,      //!< Remove channel(s) if unused.  Optional for user code.
        Drop,       //!< Remove channel(s) unconditionally.  Prevents reuse of open channel(s).
        Disconnect, //!< Remove channels(s) unconditionally, and cancel any in-progress operations.
    };

    /** Channel cache maintenance.
     *
     * @param action cf. cacheAction
     *
     * @since 0.2.0 'name' and 'action' arguments.  Defaults to previous behavior.
     */
    void cacheClear(const std::string& name = std::string(), cacheAction action = Clean);

    //! Ignore any search replies with these GUIDs
    //! @since 0.2.0
    void ignoreServerGUIDs(const std::vector<ServerGUID>& guids);

    //! Compile report about peers and channels
    //! @since 0.2.0
    Report report(bool zero=true) const;
#endif

    explicit operator bool() const { return pvt.operator bool(); }
    size_t use_count() const { return pvt.use_count(); }
private:
    std::shared_ptr<Pvt> pvt;
};

namespace detail {
struct PVRParser;

class PVXS_API CommonBase {
protected:
    std::shared_ptr<Context::Pvt> ctx;
    std::string _name;
    std::string _server;
    struct Req;
    std::shared_ptr<Req> req;
    unsigned _prio = 0u;
    bool _autoexec = true;
    bool _syncCancel = true;

    CommonBase() {}
    CommonBase(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) : ctx(ctx), _name(name) {}
    ~CommonBase();

    void _rawRequest(const Value&);
    void _field(const std::string& s);
    void _record(const std::string& key, const void* value, StoreType vtype);
    void _parse(const std::string& req);
    Value _buildReq() const;

    friend struct PVRParser;
};

class PVXS_API PRBase : public CommonBase {
protected:
    struct Args;
    std::shared_ptr<Args> _args;

    PRBase() {}
    PRBase(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) : CommonBase(ctx, name) {}
    ~PRBase();

    void _set(const std::string& name, const void *ptr, StoreType type, bool required);
    Value _builder(Value&& prototype) const;
    Value _uriArgs() const;
};

//! Options common to all operations
template<typename SubBuilder, typename Base>
class CommonBuilder : public Base {
protected:
    CommonBuilder() {}
    constexpr CommonBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) : Base(ctx, name) {}
    inline SubBuilder& _sb() { return static_cast<SubBuilder&>(*this); }
public:
    //! Add field to pvRequest blob.
    //! A more efficient alternative to @code pvRequest("field(name)") @endcode
    SubBuilder& field(const std::string& fld) { this->_field(fld); return _sb(); }

    /** Add a key/value option to the request.
     *
     * Well known options include:
     *
     * - queueSize : positive integer
     * - block     : bool
     * - process   : bool or string "true", "false", or "passive"
     * - pipeline  : bool
     *
     * A more efficient alternative to @code pvRequest("record[key=value]") @endcode
     */
    template<typename T>
    SubBuilder& record(const std::string& name, const T& val) {
        const typename impl::StoreAs<T>::store_t& norm(impl::StoreTransform<T>::in(val));
        this->_record(name, &norm, impl::StoreAs<T>::code);
        return _sb();
    }

    /** Parse pvRequest string.
     *
     *  Supported syntax is a list of zero or more entities
     *  separated by zero or more spaces.
     *
     *  - ``field(<fld.name>)``
     *  - ``record[<key>=\<value>]``
     */
    SubBuilder& pvRequest(const std::string& expr) { this->_parse(expr); return _sb(); }

    //! Store raw pvRequest blob.
    SubBuilder& rawRequest(const Value& r) { this->_rawRequest(r); return _sb(); }

    SubBuilder& priority(int p) { this->_prio = p; return _sb(); }
    SubBuilder& server(const std::string& s) { this->_server = s; return _sb(); }

#ifdef PVXS_EXPERT_API_ENABLED
    // for GET/PUT control whether operations automatically proceed from INIT to EXEC
    // cf. Operation::reExec()
    SubBuilder& autoExec(bool b) { this->_autoexec = b; return _sb(); }
#endif

    /** Controls whether Operation::cancel() and Subscription::cancel() synchronize.
     *
     * When true (the default) explicit or implicit cancel blocks until any
     * in progress callback has completed.  This makes safe some use of
     * references in callbacks.
     * @since 0.2.0
     */
    SubBuilder& syncCancel(bool b) { this->_syncCancel = b; return _sb(); }
};

} // namespace detail

//! Prepare a remote GET or GET_FIELD (info) operation.
//! See Context::get()
class GetBuilder : public detail::CommonBuilder<GetBuilder, detail::CommonBase> {
    std::function<void (const Value&)> _onInit;
    std::function<void(Result&&)> _result;
    bool _get = false;
    PVXS_API
    std::shared_ptr<Operation> _exec_info();
    PVXS_API
    std::shared_ptr<Operation> _exec_get();
public:
    GetBuilder() {}
    GetBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name, bool get) :CommonBuilder{ctx,name}, _get(get) {}
    //! Callback through which result Value or an error will be delivered.
    //! The functor is stored in the Operation returned by exec().
    GetBuilder& result(std::function<void(Result&&)>&& cb) { _result = std::move(cb); return *this; }

#ifdef PVXS_EXPERT_API_ENABLED
    // called during operation INIT phase for Get/Put/Monitor when remote type
    // description is available.
    GetBuilder& onInit(std::function<void (const Value&)>&& cb) { this->_onInit = std::move(cb); return *this; }
#endif

    /** Execute the network operation.
     *  The caller must keep returned Operation pointer until completion
     *  or the operation will be implicitly canceled.
     */
    inline std::shared_ptr<Operation> exec() {
        return _get ? _exec_get() : _exec_info();
    }

    friend struct Context::Pvt;
    friend class Context;
};
GetBuilder Context::info(const std::string& name) { return GetBuilder{pvt, name, false}; }
GetBuilder Context::get(const std::string& name) { return GetBuilder{pvt, name, true}; }

//! Prepare a remote PUT operation
//! See Context::put()
class PutBuilder : public detail::CommonBuilder<PutBuilder, detail::PRBase> {
    std::function<void (const Value&)> _onInit;
    std::function<Value(Value&&)> _builder;
    std::function<void(Result&&)> _result;
    bool _doGet = true;
public:
    PutBuilder() {}
    PutBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) :CommonBuilder{ctx,name} {}

    /** If fetchPresent is true (the default).  Then the Value passed to
     *  the build() callback will be initialized with a previous value for this PV.
     *
     *  This will be necessary for situation like NTEnum to fetch the choices list.
     *  But may be undesirable when writing to array fields to avoid
     *  the expense of fetching a copy of the array to be overwritten.
     */
    PutBuilder& fetchPresent(bool f) { _doGet = f; return *this; }

    PutBuilder& set(const std::string& name, const void *ptr, StoreType type, bool required) {
        _set(name, ptr, type, required);
        return *this;
    }

    /** Utilize default .build() to assign a value to the named field.
     *
     * @param name The field name to attempt to assign.
     * @param val The value to assign.  cf. Value::from()
     * @param required Whether to fail if this value can not be assigned to this field.
     */
    template<typename T>
    PutBuilder& set(const std::string& name, const T& val, bool required=true)
    {
        const typename impl::StoreAs<T>::store_t& norm(impl::StoreTransform<T>::in(val));
        return set(name, &norm, impl::StoreAs<T>::code, required);
    }

    /** Provide the builder callback.
     *
     *  Once the PV type information is received from the server,
     *  this function will be responsible for populating a Value
     *  which will actually be sent.
     *
     *  The functor is stored in the Operation returned by exec().
     */
    PutBuilder& build(std::function<Value(Value&&)>&& cb) { _builder = std::move(cb); return *this; }

    /** Provide the operation result callback.
     *  This callback will be passed a Result which is either an empty Value (success)
     *  or an exception on error.
     *
     *  The functor is stored in the Operation returned by exec().
     */
    PutBuilder& result(std::function<void(Result&&)>&& cb) { _result = std::move(cb); return *this; }

#ifdef PVXS_EXPERT_API_ENABLED
    // called during operation INIT phase for Get/Put/Monitor when remote type
    // description is available.
    PutBuilder& onInit(std::function<void (const Value&)>&& cb) { this->_onInit = std::move(cb); return *this; }
#endif

    /** Execute the network operation.
     *  The caller must keep returned Operation pointer until completion
     *  or the operation will be implicitly canceled.
     */
    PVXS_API
    std::shared_ptr<Operation> exec();

    friend struct Context::Pvt;
    friend class Context;
};
PutBuilder Context::put(const std::string& name) { return PutBuilder{pvt, name}; }

//! Prepare a remote RPC operation.
//! See Context::rpc()
class RPCBuilder : public detail::CommonBuilder<RPCBuilder, detail::PRBase> {
    Value _argument;
    std::function<void(Result&&)> _result;
public:
    RPCBuilder() {}
    RPCBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) :CommonBuilder{ctx,name} {}
    //! Callback through which result Value or an error will be delivered.
    //! The functor is stored in the Operation returned by exec().
    RPCBuilder& result(std::function<void(Result&&)>&& cb) { _result = std::move(cb); return *this; }

    RPCBuilder& arg(const std::string& name, const void *ptr, StoreType type) {
        _set(name, ptr, type, true);
        return *this;
    }

    /** Provide argument value.
     *
     * @param name Argument name
     * @param val The value to assign.  cf. Value::from()
     */
    template<typename T>
    RPCBuilder& arg(const std::string& name, const T& val)
    {
        const typename impl::StoreAs<T>::store_t& norm(impl::StoreTransform<T>::in(val));
        _set(name, &norm, impl::StoreAs<T>::code, true);
        return *this;
    }

    /** Execute the network operation.
     *  The caller must keep returned Operation pointer until completion
     *  or the operation will be implicitly canceled.
     */
    PVXS_API
    std::shared_ptr<Operation> exec();

    friend struct Context::Pvt;
    friend class Context;
};
RPCBuilder Context::rpc(const std::string& name) { return RPCBuilder{pvt, name}; }
RPCBuilder Context::rpc(const std::string& name, const Value &arg) {
    RPCBuilder ret{pvt, name};
    ret._argument = arg;
    return ret;
}

//! Prepare a remote subscription
//! See Context::monitor()
class MonitorBuilder : public detail::CommonBuilder<MonitorBuilder, detail::CommonBase> {
    std::function<void(Subscription&, const Value&)> _onInit;
    std::function<void(Subscription&)> _event;
    bool _maskConn = true;
    bool _maskDisconn = false;
public:
    MonitorBuilder() {}
    MonitorBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& name) :CommonBuilder{ctx,name} {}
    /** Install FIFO not-empty event callback.
     *
     *  This functor will be called each time the Subscription event queue becomes
     *  not empty.  A Subscription becomes empty when Subscription::pop() returns
     *  an empty/invalid Value.
     *
     *  The functor is stored in the Subscription returned by exec().
     */
    MonitorBuilder& event(std::function<void(Subscription&)>&& cb) { _event = std::move(cb); return *this; }
    //! Include Connected exceptions in queue (default false).
    MonitorBuilder& maskConnected(bool m = true) { _maskConn = m; return *this; }
    //! Include Disconnected exceptions in queue (default true).
    MonitorBuilder& maskDisconnected(bool m = true) { _maskDisconn = m; return *this; }

#ifdef PVXS_EXPERT_API_ENABLED
    // called during operation INIT phase for Get/Put/Monitor when remote type
    // description is available.
    MonitorBuilder& onInit(std::function<void (Subscription&, const Value&)>&& cb) { this->_onInit = std::move(cb); return *this; }
#endif

    //! Submit request to subscribe
    PVXS_API
    std::shared_ptr<Subscription> exec();

    friend struct Context::Pvt;
    friend class Context;
};
MonitorBuilder Context::monitor(const std::string& name) { return MonitorBuilder{pvt, name}; }

class RequestBuilder : public detail::CommonBuilder<RequestBuilder, detail::CommonBase>
{
public:
    RequestBuilder() {}
    //! Return composed pvRequest
    Value build() const {
        return _buildReq();
    }
};
RequestBuilder Context::request() { return RequestBuilder{}; }

//! cf. Context::connect()
//! @since 0.2.0
class ConnectBuilder
{
    std::shared_ptr<Context::Pvt> ctx;
    std::string _pvname;
    std::string _server;
    std::function<void()> _onConn;
    std::function<void()> _onDis;
    bool _syncCancel = true;
public:
    ConnectBuilder() {}
    ConnectBuilder(const std::shared_ptr<Context::Pvt>& ctx, const std::string& pvname)
        :ctx(ctx)
        ,_pvname(pvname)
    {}

    //! Handler to be invoked when channel becomes connected.
    ConnectBuilder& onConnect(std::function<void()>&& cb) { _onConn = std::move(cb); return *this; }
    //! Handler to be invoked when channel becomes disconnected.
    ConnectBuilder& onDisconnect(std::function<void()>&& cb) { _onDis = std::move(cb); return *this; }

    /** Controls whether Connect::~Connect() synchronizes.
     *
     * When true (the default) explicit or implicit cancel blocks until any
     * in progress callback has completed.  This makes safe some use of
     * references in callbacks.
     * @since 0.2.0
     */
    ConnectBuilder& syncCancel(bool b) { this->_syncCancel = b; return *this; }

    ConnectBuilder& server(const std::string& s) { this->_server = s; return *this; }

    //! Submit request to connect
    PVXS_API
    std::shared_ptr<Connect> exec();
};
ConnectBuilder Context::connect(const std::string& pvname) { return ConnectBuilder{pvt, pvname}; }

//! Change of state event associated with a Context::discover()
struct Discovered {
    //! What sort of event is this?
    enum event_t : uint8_t {
        Online=1,     //!< Beacon from new server GUID
        Timeout=2,    //!< Beacon timeout for previous server
    } event;
    uint8_t peerVersion; //!< Last reported peer PVA protocol version.
    std::string peer;  //!< source of Beacon
    std::string proto; //!< Advertised protocol.  eg. "tcp"
    std::string server;//!< Server protocol endpoint.
    ServerGUID guid;   //!< Server provided ID
    epicsTime time;    //!< Local system time of event
};
PVXS_API
std::ostream& operator<<(std::ostream& strm, const Discovered& evt);
//! Prepare a Context::discover() operation
//! @since 0.3.0
class DiscoverBuilder
{
    std::shared_ptr<Context::Pvt> ctx;
    std::function<void(const Discovered &)> _fn;
    bool _syncCancel = true;
    bool _ping = false;
public:
    DiscoverBuilder(const std::shared_ptr<Context::Pvt>& ctx, std::function<void(const Discovered &)>&& fn)
        :ctx(ctx)
        ,_fn(fn)
    {}

    /** Controls whether client will actively seek to immediately discover all servers.
     *
     * If false, then client will only wait for servers to periodically announce themselves.
     */
    DiscoverBuilder& pingAll(bool b) { this->_ping = b; return *this; }

    /** Controls whether Operation::cancel() synchronizes.
     *
     * When true (the default) explicit or implicit cancel blocks until any
     * in progress callback has completed.  This makes safe some use of
     * references in callbacks.
     */
    DiscoverBuilder& syncCancel(bool b) { this->_syncCancel = b; return *this; }

    //! Execute.  The returned Operation will never complete.
    PVXS_API
    std::shared_ptr<Operation> exec();
};
DiscoverBuilder Context::discover(std::function<void (const Discovered &)> && fn) { return DiscoverBuilder(pvt, std::move(fn)); }

struct PVXS_API Config : public impl::ConfigCommon {
    /** List of unicast, multicast, and broadcast addresses to which search requests will be sent.
     *
     * Entries may take the forms:
     * - ``<ipaddr>[:<port#>]``
     * - ``<ipmultiaddr>[:<port>][,<ttl>][@<ifaceaddr>]``
     */
    std::vector<std::string> addressList;

    //! List of local interface addresses on which beacons may be received.
    //! Also constrains autoAddrList to only consider broadcast addresses of listed interfaces.
    //! Empty implies wildcard 0.0.0.0
    std::vector<std::string> interfaces;

    //! List of TCP name servers.
    //! Client context will maintain connections, and send search requests, to these servers.
    //! @since 0.2.0
    std::vector<std::string> nameServers;

    //! Whether to extend the addressList with local interface broadcast addresses.  (recommended)
    bool autoAddrList = true;

private:
    bool BE = EPICS_BYTE_ORDER==EPICS_ENDIAN_BIG;
    bool UDP = true;
public:

    // compat
    static inline Config from_env() { return Config{}.applyEnv(); }

    //! Default configuration using process environment
    static inline Config fromEnv()  { return Config{}.applyEnv(); }

    //! update using defined EPICS_PVA* environment variables
    Config& applyEnv();

    typedef std::map<std::string, std::string> defs_t;
    //! update with definitions as with EPICS_PVA* environment variables
    //! Process environment is not changed.
    Config& applyDefs(const defs_t& defs);

    //! extract definitions with environment variable names as keys.
    //! Process environment is not changed.
    void updateDefs(defs_t& defs) const;

    /** Apply rules to translate current requested configuration
     *  into one which can actually be loaded based on current host network configuration.
     *
     *  Explicit use of expand() is optional as the Context ctor expands any Config given.
     *  expand() is provided as a aid to help understand how Context::effective() is arrived at.
     *
     *  @post autoAddrList==false
     */
    void expand();

    //! Create a new client Context using the current configuration.
    inline
    Context build() const {
        return Context(*this);
    }

#ifdef PVXS_EXPERT_API_ENABLED
    // for protocol compatibility testing
    inline Config& overrideSendBE(bool be) { BE = be; return *this; }
    inline bool sendBE() const { return BE; }
    inline Config& overrideShareUDP(bool share) { UDP = share; return *this; }
    inline bool shareUDP() const { return UDP; }
#endif
};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const Config& conf);

} // namespace client
} // namespace pvxs

#endif // PVXS_CLIENT_H
