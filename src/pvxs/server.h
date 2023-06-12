/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#ifndef PVXS_SERVER_H
#define PVXS_SERVER_H

#include <osiSock.h>

#include <iosfwd>
#include <functional>
#include <string>
#include <tuple>
#include <set>
#include <map>
#include <vector>
#include <memory>
#include <array>

#include <epicsEndian.h>

#include <pvxs/version.h>
#include <pvxs/util.h>
#include <pvxs/data.h>
#include <pvxs/netcommon.h>

namespace pvxs {
namespace client {
struct Config;
}
namespace server {

struct SharedPV;
struct Source;
struct Config;

/** PV Access protocol server instance
 *
 * Use a Config to determine how this server will bind, listen,
 * and announce itself.
 *
 * In order to be useful, a Server will have one or more Source instances added
 * to it with addSource().
 *
 * As a convenience, each Server instance automatically contains a "__builtin" StaticSource
 * to which SharedPV instances can be directly added.
 * The "__builtin" has priority zero, and can be accessed or even removed like any Source
 * explicitly added with addSource().
 *
 * There is also a "__server" source which provides the special "server" PV
 * used by the pvlist CLI.
 */
class PVXS_API Server
{
public:

    //! An empty/dummy Server
    constexpr Server() = default;
    //! Create/allocate, but do not start, a new server with the provided config.
    explicit Server(const Config&);
    Server(const Server&) = default;
    Server(Server&& o) = default;
    Server& operator=(const Server&) = default;
    Server& operator=(Server&& o) = default;
    ~Server();

    /** Create new server based on configuration from $EPICS_PVA* environment variables.
     *
     * Shorthand for @code Config::fromEnv().build() @endcode.
     * @since 0.2.1
     */
    static
    Server fromEnv();

    //! Begin serving.  Does not block.
    Server& start();
    //! Stop server
    Server& stop();

    /** start() and then (maybe) stop()
     *
     * run() may be interrupted by calling interrupt(),
     * or by SIGINT or SIGTERM (only one Server per process)
     *
     * Intended to simple CLI programs.
     * Only one Server in a process may be in run() at any moment.
     * Other use case should call start()/stop()
     */
    Server& run();
    //! Queue a request to break run()
    Server& interrupt();

    //! effective config
    const Config& config() const;

    //! Create a client configuration which can communicate with this Server.
    //! Suitable for use in self-contained unit-tests.
    client::Config clientConfig() const;

    //! Add a SharedPV to the "__builtin" StaticSource
    Server& addPV(const std::string& name, const SharedPV& pv);
    //! Remove a SharedPV from the "__builtin" StaticSource
    Server& removePV(const std::string& name);

    //! Add a Source to this server with an arbitrary source name.
    //!
    //! Source names beginning with "__" are reserved for internal use.
    //! eg. "__builtin" and "__server".
    //!
    //! @param name Source name
    //! @param src The Source.  A strong reference to this Source which will be released by removeSource() or ~Server()
    //! @param order Determines the order in which this Source::onCreate() will be called.  Lowest first.
    //!
    //! @throws std::runtime_error If this (name, order) has already been added.
    Server& addSource(const std::string& name,
                      const std::shared_ptr<Source>& src,
                      int order =0);

    //! Disassociate a Source using the name and priority given to addSource()
    std::shared_ptr<Source> removeSource(const std::string& name,
                                         int order =0);

    //! Fetch a previously added Source.
    std::shared_ptr<Source> getSource(const std::string& name,
                                      int order =0);

    //! List all source names and priorities.
    std::vector<std::pair<std::string, int> > listSource();

#ifdef PVXS_EXPERT_API_ENABLED
    //! Compile report about peers and channels
    //! @param zero If true, zero counters after reading
    //! @since 0.2.0
    Report report(bool zero=true) const;
#endif

    explicit operator bool() const { return !!pvt; }

    friend
    PVXS_API
    std::ostream& operator<<(std::ostream& strm, const Server& serv);

    struct Pvt;
private:
    std::shared_ptr<Pvt> pvt;
};

PVXS_API
std::ostream& operator<<(std::ostream& strm, const Server& serv);

//! Configuration for a Server
struct PVXS_API Config : public impl::ConfigCommon {
    //! List of network interface addresses (**not** host names) to which this server will bind.
    //! interfaces.empty() treated as an alias for "0.0.0.0", which may also be given explicitly.
    //! Port numbers are optional and unused (parsed and ignored)
    std::vector<std::string> interfaces;
    //! Ignore client requests originating from addresses in this list.
    //! Entries must be IP addresses with optional port numbers.
    //! Port number zero (default) is treated as a wildcard which matches any port.
    //! @since 0.2.0
    std::vector<std::string> ignoreAddrs;
    //! Addresses (**not** host names) to which (UDP) beacons message will be sent.
    //! May include broadcast and/or unicast addresses.
    //! Supplemented only if auto_beacon==true
    std::vector<std::string> beaconDestinations;
    //! Whether to populate the beacon address list automatically.  (recommended)
    bool auto_beacon = true;

    //! Server unique ID.  Only meaningful in readback via Server::config()
    ServerGUID guid{};

private:
    bool BE = EPICS_BYTE_ORDER==EPICS_ENDIAN_BIG;
    bool UDP = true;
public:

    // compat
    static inline Config from_env() { return Config{}.applyEnv(); }

    //! Default configuration using process environment
    static inline Config fromEnv()  { return Config{}.applyEnv(); }

    //! Configuration limited to the local loopback interface on a randomly chosen port.
    //! Suitable for use in self-contained unit-tests.
    //! @since 0.3.0 Address family argument added.
    static Config isolated(int family=AF_INET);

    //! update using defined EPICS_PVA* environment variables
    Config& applyEnv();

    typedef std::map<std::string, std::string> defs_t;
    //! update with definitions as with EPICS_PVA* environment variables.
    //! Process environment is not changed.
    Config& applyDefs(const defs_t& def);

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

    //! Create a new Server using the current configuration.
    inline Server build() const {
        return Server(*this);
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

}} // namespace pvxs::server

#endif // PVXS_SERVER_H
