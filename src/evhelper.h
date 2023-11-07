/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef EVHELPER_H
#define EVHELPER_H

#include <sstream>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <set>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>

#ifdef PVXS_ENABLE_OPENSSL
#  include <event2/bufferevent_ssl.h>
#endif

#include <pvxs/version.h>
#include <utilpvt.h>

#include <epicsTime.h>

#include "pvaproto.h"
#ifdef PVXS_ENABLE_OPENSSL
#  include "ossl.h"
#endif

// hooks for std::unique_ptr
namespace std {
template<>
struct default_delete<event_config> {
    inline void operator()(event_config* ev) { event_config_free(ev); }
};
template<>
struct default_delete<event_base> {
    inline void operator()(event_base* ev) { event_base_free(ev); }
};
template<>
struct default_delete<event> {
    inline void operator()(event* ev) { event_free(ev); }
};
template<>
struct default_delete<evconnlistener> {
    inline void operator()(evconnlistener* ev) { evconnlistener_free(ev); }
};
template<>
struct default_delete<bufferevent> {
    inline void operator()(bufferevent* ev) { bufferevent_free(ev); }
};
template<>
struct default_delete<evbuffer> {
    inline void operator()(evbuffer* ev) { evbuffer_free(ev); }
};
}

namespace pvxs {namespace impl {

template<typename T>
struct ev_delete;
#define DEFINE_DELETE(TYPE) \
template<> struct ev_delete<TYPE> { \
    inline void operator()(TYPE* ev) { TYPE ## _free(ev); } \
}
DEFINE_DELETE(event_config);
DEFINE_DELETE(event_base);
DEFINE_DELETE(event);
DEFINE_DELETE(evconnlistener);
DEFINE_DELETE(bufferevent);
DEFINE_DELETE(evbuffer);
#undef DEFINE_DELETE

//! unique_ptr which is never constructed with NULL
template<typename T, typename D>
struct owned_ptr : public std::unique_ptr<T, D>
{
    typedef std::unique_ptr<T, D> base_t;
    constexpr owned_ptr() {}
    constexpr owned_ptr(std::nullptr_t np) : base_t(np) {}
    explicit owned_ptr(const char* file, int line, T* ptr) : base_t(ptr) {
        if(!*this)
            throw loc_bad_alloc(file, line);
    }

    // for functions which return a pointer in an argument
    //   int some(T** presult); // store *presult = output
    // use like
    //   owned_ptr<T> x;
    //   some(x.acquire());
    struct acquisition {
        base_t* o;
        T* ptr = nullptr;
        operator T** () { return &ptr; }
        constexpr acquisition(base_t* o) :o(o) {}
        ~acquisition() {
            o->reset(ptr);
        }
    };
    acquisition acquire() { return acquisition{this}; }
};

/* It seems that std::function<void()>(Fn&&) from gcc (circa 8.3) and clang (circa 7.0)
 * always copies the functor/lambda.  We can't allow this when transferring ownership
 * of shared_ptr<> instances to a worker thread as it leaves the caller thread with a
 * reference.
 *
 * std::unique_ptr<int> arg{new int(42)};
 * // eg. with
 * auto lambda = [arg{std::move(arg)}]() { // c++14 capture w/ move
 *     auto trash(std::move(arg));
 * };
 * // or
 * auto lambda = std::bind([](std::unique_ptr<int>& arg) { // c++11 capture w/ move
 *     auto trash(std::move(arg));
 * }, std::move(arg));
 * // the following line tries to copy the unique_ptr which fails to compile
 * std::function<void()> fn(std::move(lambda));
 *
 * So we invent our own limited, non-copyable, version of std::function<void()>.
 */
namespace mdetail {
struct PVXS_API VFunctor0 {
    VFunctor0() = default;
    VFunctor0(VFunctor0&) = delete;
    VFunctor0(const VFunctor0&) = delete;
    VFunctor0& operator=(const VFunctor0&) = delete;
    virtual ~VFunctor0() =0;
    virtual void invoke() =0;
};
template<typename Fn>
struct Functor0 final : public VFunctor0 {
    Functor0() = default;
    Functor0(Fn&& fn) : fn(std::move(fn)) {}
    virtual ~Functor0() {}

    void invoke() override final { fn(); }
private:
    Fn fn;
};
} // namespace detail

struct mfunction {
    mfunction() = default;
    template<typename Fn>
    mfunction(Fn&& fn)
        :fn{new mdetail::Functor0<Fn>(std::move(fn))}
    {}
    void operator()() const {
        fn->invoke();
    }
    explicit operator bool() const {
        return fn.operator bool();
    }
private:
    std::unique_ptr<mdetail::VFunctor0> fn;
};

struct PVXS_API evbase {
    evbase() = default;
    explicit evbase(const std::string& name, unsigned prio=0);
    ~evbase();

    evbase internal() const;

    void join() const;

    void sync() const;

private:
    bool _dispatch(mfunction&& fn, bool dothrow) const;
    bool _call(mfunction&& fn, bool dothrow) const;
public:

    // queue request to execute in event loop.  return after executed.
    inline
    void call(mfunction&& fn) const {
        _call(std::move(fn), true);
    }
    inline
    bool tryCall(mfunction&& fn) const {
        return _call(std::move(fn), false);
    }

    // queue request to execute in event loop.  return immediately.
    inline
    void dispatch(mfunction&& fn) const {
        _dispatch(std::move(fn), true);
    }
    inline
    bool tryDispatch(mfunction&& fn) const {
        return _dispatch(std::move(fn), false);
    }

    bool tryInvoke(bool docall, mfunction&& fn) const {
        if(docall)
            return tryCall(std::move(fn));
        else
            return tryDispatch(std::move(fn));
    }

    void assertInLoop() const;
    //! Caller must be on the worker, or the worker must be stopped.
    //! @returns true if working is running.
    bool assertInRunningLoop() const;

    inline void reset() { pvt.reset(); }

private:
    struct Pvt;
    std::shared_ptr<Pvt> pvt;
public:
    event_base* base = nullptr;
};

template<typename T>
using ev_owned_ptr = owned_ptr<T, ev_delete<T>>;
typedef ev_owned_ptr<event_config> evconfig;
typedef ev_owned_ptr<event_base> evbaseptr;
typedef ev_owned_ptr<event> evevent;
typedef ev_owned_ptr<evconnlistener> evlisten;
typedef ev_owned_ptr<bufferevent> evbufferevent;
typedef ev_owned_ptr<evbuffer> evbuf;

PVXS_API
void to_wire(Buffer& buf, const SockAddr& val);

PVXS_API
void from_wire(Buffer &buf, SockAddr& val);

struct PVXS_API evsocket
{
    evutil_socket_t sock;
    int af;

    // default construct an invalid socket
    constexpr evsocket() noexcept :sock(-1), af(AF_UNSPEC) {}

    // construct from a valid (not -1) socket
    explicit evsocket(int af, evutil_socket_t sock, bool blocking=false);

    // create a new socket
    evsocket(int af, int type, int proto, bool blocking=false);

    // movable
    evsocket(evsocket&& o) noexcept;
    evsocket& operator=(evsocket&&) noexcept;

    // not copyable
    evsocket(const evsocket&) = delete;
    evsocket& operator=(const evsocket&) = delete;

    ~evsocket();

    SockAddr sockname() const;

    // test validity
    inline operator bool() const { return sock!=-1; }

    void bind(const SockAddr& addr) const;
    void bind(SockAddr& addr) const;

    void listen(int backlog) const;

    void set_broadcast(bool b) const;

    //! Join multicast group, optionally on selected interface
    bool mcast_join(const MCastMembership& m) const;
    //! Reverse previous join
    void mcast_leave(const MCastMembership& m) const;
    //! Prepare socket for subsequent sendto() with TTL and output interface
    void mcast_prep_sendto(const SockEndpoint& ep) const;

    //! Whether mcasts sent from this socket should be received to local listeners
    //! @see IP_MULTICAST_LOOP and IPV6_MULTICAST_LOOP
    void mcast_loop(bool loop) const;
    //! Disable IPv4 through IPv6 socket
    void ipv6_only(bool b=true) const;

    //! Linux specific include OS dropped packet counter as cmsg
    void enable_SO_RXQ_OVFL() const;

    void enable_IP_PKTINFO() const;

    //! wraps osiSockDiscoverBroadcastAddresses()
    std::vector<SockAddr> broadcasts(const SockAddr* match=nullptr) const;

    static
    size_t get_buffer_size(evutil_socket_t sock, bool tx);

    static
    bool canIPv6;

    static bool init_canIPv6() noexcept;

    enum ipstack_t {
        Linsock,
        Winsock,
        GenericBSD,
    };
    static ipstack_t ipstack;
};

struct PVXS_API IfaceMap {
    static
    IfaceMap& instance();
    static
    void cleanup();

    IfaceMap();

    // return true if ifindex is valid, and addr an interface address assigned to it.
    bool has_address(uint64_t ifindex, const SockAddr& addr);
    // lookup interface name by index
    std::string name_of(uint64_t index);
    // find (an) interface name with this address.  useful for IPv4.  returns empty string if not found.
    std::string name_of(const SockAddr& addr);
    // returns 0 if not found
    uint64_t index_of(const std::string& name);
    // is this a valid interface or broadcast address?
    bool is_iface(const SockAddr& addr);
    // is this a valid interface or broadcast address?
    bool is_broadcast(const SockAddr& addr);
    // look up interface address.  useful for IPV4.  returns AF_UNSPEC if not found
    SockAddr address_of(const std::string& name);
    // all interface names except LO
    std::set<std::string> all_external();

    // caller must hold lock
    void refresh(bool force=false);

    struct Iface {
        std::string name;
        uint64_t index;
        bool isLO;
        // interface address(s) -> (maybe) broadcast addr
        std::map<SockAddr, SockAddr, SockAddrOnlyLess> addrs;
        Iface(const std::string& name, uint64_t index, bool isLO) :name(name), index(index), isLO(isLO) {}
    };

    SockAttach attach;
    epicsMutex lock;
    std::map<uint64_t, Iface> byIndex;
    std::map<std::string, Iface*> byName;
    // map address to tuple of interface and broadcast?
    std::multimap<SockAddr, std::pair<Iface*, bool>, SockAddrOnlyLess> byAddr;
    epicsTime updated;
private:
    static
    decltype (byIndex) _refresh();
};

} // namespace impl


#ifdef PVXS_EXPERT_API_ENABLED

struct Timer::Pvt {
    const evbase base;
    std::function<void()> cb;
    evevent timer;

    Pvt(const evbase& base, std::function<void()>&& cb)
        :base(base), cb(std::move(cb))
    {}
    ~Pvt();

    bool cancel();

    static
    Timer buildOneShot(double delay, const evbase &base, std::function<void()>&& cb);

    INST_COUNTER(Timer);
};

#endif // PVXS_EXPERT_API_ENABLED

} // namespace pvxs

#endif /* EVHELPER_H */
