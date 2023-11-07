/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#ifndef OSISOCKEXT_H
#define OSISOCKEXT_H

#include <osiSock.h>

#include <string>
#include <string.h>

#include <event2/util.h>

#include <pvxs/version.h>

// added with Base 3.15
#ifndef SOCK_EADDRNOTAVAIL
#  ifdef _WIN32
#    define SOCK_EADDRNOTAVAIL WSAEADDRNOTAVAIL
#  else
#    define SOCK_EADDRNOTAVAIL EADDRNOTAVAIL
#  endif
#endif

namespace pvxs {
namespace impl {
struct ConfigCommon;
} // namespace impl

PVXS_API
void osiSockAttachExt();

struct SockAttach {
    SockAttach() { osiSockAttachExt(); }
    SockAttach(const SockAttach&) = delete;
    SockAttach& operator=(const SockAttach&) = delete;
    ~SockAttach() { osiSockRelease(); }
};

//! representation of a network address
struct PVXS_API SockAddr {
    union store_t {
        sockaddr sa;
        sockaddr_in in;
#ifdef AF_INET6
        sockaddr_in6 in6;
#endif
    };
private:
    store_t  store;
public:

    explicit SockAddr(int af = AF_UNSPEC);
    explicit SockAddr(const char *address, unsigned short port=0);
    explicit SockAddr(const sockaddr *addr, socklen_t alen=0);
    inline explicit SockAddr(const std::string& address, unsigned short port=0) :SockAddr(address.c_str(), port) {}

    size_t size() const noexcept;
    inline
    size_t capacity() const { return sizeof(store); }

    inline unsigned short family() const noexcept { return store.sa.sa_family; }
    unsigned short port() const noexcept;
    void setPort(unsigned short port);
    SockAddr withPort(unsigned short port) const {
        SockAddr temp(*this);
        temp.setPort(port);
        return temp;
    }

    void setAddress(const char *, unsigned short port=0);
    inline void setAddress(const std::string& s, unsigned short port=0) {
        setAddress(s.c_str(), port);
    }

    bool isAny() const noexcept;
    bool isLO() const noexcept;
    bool isMCast() const noexcept;

    SockAddr map4to6() const;

    store_t* operator->() { return &store; }
    const store_t* operator->() const { return &store; }

    std::string tostring() const;

    static SockAddr any(int af, unsigned port=0);
    static SockAddr loopback(int af, unsigned port=0);

    inline int compare(const SockAddr& o, bool useport=true) const {
        return evutil_sockaddr_cmp(&store.sa, &o.store.sa, useport);
    }

    inline bool operator<(const SockAddr& o) const {
        return evutil_sockaddr_cmp(&store.sa, &o.store.sa, true)<0;
    }
    inline bool operator==(const SockAddr& o) const {
        return evutil_sockaddr_cmp(&store.sa, &o.store.sa, true)==0;
    }
    inline bool operator!=(const SockAddr& o) const {
        return !(*this==o);
    }
};

// compare address only, ignore port number
struct SockAddrOnlyLess {
    bool operator()(const SockAddr& lhs, const SockAddr& rhs) const {
        return lhs.compare(rhs, false)<0;
    }
};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const SockAddr& addr);

// resolved multicast group membership request
struct MCastMembership {
    int af = AF_UNSPEC;
    union {
        ip_mreq in;
        ipv6_mreq in6;
    } req{};
    bool operator<(const MCastMembership& o) const {
        if(af==o.af) {
            if(af==AF_INET)
                return memcmp(&req.in, &o.req.in, sizeof(o.req.in))<0;
            else
                return memcmp(&req.in6, &o.req.in6, sizeof(o.req.in6))<0;
        }
        return af<o.af;
    }
};

/** search/beacon destination
 *
 *  <IP46>
 *  <IP46>,<ttl#>
 *  <IP46>@iface
 *  <IP46>,<ttl#>@iface
 */
struct PVXS_API SockEndpoint {
    SockAddr addr; // ucast, mcast, or bcast
    // if mcast, then output TTL and interface
    int ttl=-1;
    std::string iface;
    enum ep_t : uint8_t {
        Plain, // "classic" PVA in the clear
        TLS,   // PVA over TLS
    } scheme = Plain;

    SockEndpoint() = default;
    SockEndpoint(const char* ep, const impl::ConfigCommon *conf = nullptr, uint16_t defport=0);
    SockEndpoint(const std::string& ep, const impl::ConfigCommon *conf = nullptr, uint16_t defport=0)
        :SockEndpoint(ep.c_str(), conf, defport) {}
    explicit SockEndpoint(const SockAddr& addr) :addr(addr) {}

    MCastMembership resolve() const;
};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const SockEndpoint& addr);

PVXS_API
bool operator==(const SockEndpoint& lhs, const SockEndpoint& rhs);

inline
bool operator!=(const SockEndpoint& lhs, const SockEndpoint& rhs) { return !(lhs==rhs); }

struct GetAddrInfo {
    explicit GetAddrInfo(const char *name);
    inline explicit GetAddrInfo(const std::string& name) :GetAddrInfo(name.c_str()) {}
    GetAddrInfo(const GetAddrInfo&) = delete;
    inline
    GetAddrInfo(GetAddrInfo&& o) :info(o.info) {
        o.info = nullptr;
    }
    ~GetAddrInfo();

    struct iterator {
        evutil_addrinfo *pos = nullptr;
        inline iterator() = default;
        inline iterator(evutil_addrinfo *pos) :pos(pos) {}
        inline SockAddr operator*() const {
            return SockAddr(pos->ai_addr, pos->ai_addrlen);
        }
        inline iterator& operator++() {
            pos = pos->ai_next;
            return *this;
        }
        inline iterator operator++(int) {
            auto ret(*this);
            pos = pos->ai_next;
            return ret;
        }
        inline bool operator==(const iterator& o) const {
            return pos==o.pos;
        }
        inline bool operator!=(const iterator& o) const {
            return pos!=o.pos;
        }
    };

    inline iterator begin() const { return iterator{info}; }
    inline iterator end() const { return iterator{}; }

private:
    evutil_addrinfo *info;
};

struct recvfromx {
    evutil_socket_t sock;
    void *buf;
    size_t buflen;
    SockAddr* src;
    SockAddr* dst;  // if enable_IP_PKTINFO()
    int64_t dstif;  // if enable_IP_PKTINFO(), destination interface index
    uint32_t ndrop; // if enable_SO_RXQ_OVFL()

    PVXS_API
    int call();
};

} // namespace pvxs

#endif // OSISOCKEXT_H
