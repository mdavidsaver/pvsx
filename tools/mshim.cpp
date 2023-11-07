/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <map>
#include <vector>
#include <iostream>
#include <string>
#include <exception>

#include <epicsVersion.h>
#include <epicsGetopt.h>

#include <pvxs/log.h>
#include <pvxs/server.h>
#include "utilpvt.h"
#include "evhelper.h"
#include "udp_collector.h"

using namespace pvxs;

DEFINE_LOGGER(applog, "mshim");

namespace {

void usage(const char* argv0)
{
    std::cerr<<
                "Usage: "<<argv0<<" [-L <ip>[@iface]]... [-F]\n"
                "\n"
                "  -L <ip>                 Interface address to listen on.\n"
                "  -L <ip>[@iface]         Join multicast group, optionally via a certain interface\n"
                "                          to override the default selected by the OS.\n"
                "  -F <ip>                 Forward received packets to destination unicast/broadcast address.\n"
                "  -F <ip>[,ttl#][@iface]  Forward received packets to destination multicast group.\n"
                "                          Optionally override OS default TTL and outbound interface selected\n"
                "                          by the OS.\n"
                "  -p <port#>              Default port number.  (overrides $EPICS_PVA_BROADCAST_PORT)\n"
                "  -h                      Show this message.\n"
                "  -V                      Show versions.\n"
                "\n"
                "  Compatibility shim for IPv4 multicast by non-aware PVA clients/servers.\n"
                "\n"
                "  Examples:\n"
                "\n"
                "    1. Forwarding searches from local clients to a multicast group via the default interface.\n"
                "    2. Forwarding beacons from multicast group via the default interface to local clients.\n"
                "\n"
                "    "<<argv0<<" -L 127.0.0.1:15076 -F 224.1.1.1,255 &  # 1\n"
                "    "<<argv0<<" -L 224.1.1.1,255 -F 127.0.0.1:15076 &  # 2\n"
    <<std::endl;
}

SockEndpoint parseEP(const char* optarg, const server::Config& conf)
{
    SockEndpoint ep;
    try {
        ep = SockEndpoint(optarg, nullptr, conf.udp_port);

    }catch(std::exception& e){
        std::cerr<<"Error: Invalid group spec. '"<<escape(optarg)<<"' : "<<e.what()<<std::endl;
        exit(1);
    }
    if(ep.addr.family()!=AF_INET) {
        std::cerr<<"Only IPv4 addresses are supported"<<std::endl;
        exit(1);

    } else if(ep.addr.port()==0) {
        std::cerr<<"Non-zero port number required"<<std::endl;
        exit(1);
    }
    return ep;
}

struct App {
    const SockAttach attach;
    IfaceMap& ifmap;
    const evsocket sockTx{AF_INET, SOCK_DGRAM, 0};
    std::vector<SockEndpoint> destinations;

    // effectively local to UDPManager worker
    std::vector<uint8_t> scratch;

    App()
        :ifmap(IfaceMap::instance())
        ,scratch(0x10000)
    {
        auto bind_addr(SockAddr::any(sockTx.af));
        sockTx.bind(bind_addr);
        sockTx.mcast_loop(true);
    }

    void onSearch(const UDPManager::Search& msg)
    {
        FixedBuf buf(true, scratch);
        auto save_header = buf.save();
        buf._skip(8);
        to_wire(buf, msg.searchID);
        auto save_flags = buf.save();
        to_wire(buf, {
                    uint8_t(msg.mustReply ? pva_search_flags::MustReply : 0u),
                    0,0,0
                });
        assert(!msg.server.isAny() && msg.server.family()==AF_INET); // UDPManager has already handled this case
        to_wire(buf, msg.server);
        to_wire(buf, uint16_t(msg.server.port()));

        size_t nproto = msg.otherproto.size();
        if(msg.protoTCP)
            nproto++;
        if(msg.protoTLS)
            nproto++;

        to_wire(buf, Size{nproto});
        if(msg.protoTLS)
            to_wire(buf, "tls");
        if(msg.protoTCP)
            to_wire(buf, "tcp");
        for(auto& prot : msg.otherproto) {
            to_wire(buf, prot);
        }

        to_wire(buf, uint16_t(msg.names.size()));

        for(auto& name : msg.names) {
            to_wire(buf, name.id);
            to_wire(buf, name.name);
        }

        if(!buf.good()) {
            log_warn_printf(applog, "Unable to construct CMD_SEARCH to forward. %s:%d\n",
                            buf.file(), buf.line());
            return;
        }

        auto bufsize = buf.save()-save_header;
        {
            FixedBuf buf(true, save_header, 8u);
            to_wire(buf, Header{CMD_SEARCH, 0, uint32_t(bufsize-8u)});
        }

        for(auto& dest : destinations) {
            if(dest.addr.isMCast() || ifmap.is_broadcast(dest.addr)) {
                *save_flags &= ~pva_search_flags::Unicast;

            } else {
                *save_flags |= pva_search_flags::Unicast;
            }

            sockTx.mcast_prep_sendto(dest);

            auto ret = sendto(sockTx.sock, (char*)scratch.data(), bufsize, 0,
                              &dest.addr->sa, dest.addr.size());

            if(ret < 0) {
                int err = evutil_socket_geterror(sockTx.sock);
                if(err==SOCK_EWOULDBLOCK || err==EAGAIN || err==SOCK_EINTR) {
                    break; // too bad, better luck next time

                } else {
                    log_warn_printf(applog, "Unable to send search to %s, skip.  (%d) %s\n",
                                    std::string(SB()<<dest).c_str(), err, evutil_socket_error_to_string(err));
                }

            } else if(ret!=bufsize) {
                log_warn_printf(applog, "Sent truncated search to %s?\n",
                                std::string(SB()<<dest).c_str());

            } else {
                log_debug_printf(applog, "Forwarded search to %s -> %s -> %s?\n",
                                 msg.src.tostring().c_str(),
                                 msg.server.tostring().c_str(),
                                 std::string(SB()<<dest).c_str());
            }
        }
    }

    void onBeacon(const UDPManager::Beacon& msg)
    {
        FixedBuf buf(true, scratch);
        auto save_header = buf.save();
        buf._skip(8);
        _to_wire<12>(buf, msg.guid.data(), false, __FILE__, __LINE__);
        to_wire(buf, uint32_t(0u)); // skip flags, seq, and change count.  unused
        assert(!msg.server.isAny() && msg.server.family()==AF_INET); // UDPManager has already handled this case
        to_wire(buf, msg.server);
        to_wire(buf, uint16_t(msg.server.port()));
        to_wire(buf, msg.proto);
        // "NULL" serverStatus
        to_wire(buf, uint8_t(0xff));

        if(!buf.good()) {
            log_warn_printf(applog, "Unable to construct CMD_SEARCH to forward. %s:%d\n",
                            buf.file(), buf.line());
            return;
        }

        auto bufsize = buf.save()-save_header;
        {
            FixedBuf buf(true, save_header, 8u);
            to_wire(buf, Header{CMD_SEARCH, 0, uint32_t(bufsize-8u)});
        }

        for(auto& dest : destinations) {
            sockTx.mcast_prep_sendto(dest);

            auto ret = sendto(sockTx.sock, (char*)scratch.data(), bufsize, 0,
                              &dest.addr->sa, dest.addr.size());

            if(ret < 0) {
                int err = evutil_socket_geterror(sockTx.sock);
                if(err==SOCK_EWOULDBLOCK || err==EAGAIN || err==SOCK_EINTR) {
                    break; // too bad, better luck next time

                } else {
                    log_warn_printf(applog, "Unable to send beacon to %s, skip.  (%d) %s\n",
                                    std::string(SB()<<dest).c_str(), err, evutil_socket_error_to_string(err));
                }

            } else if(ret!=bufsize) {
                log_warn_printf(applog, "Sent truncated beacon to %s?\n",
                                std::string(SB()<<dest).c_str());

            } else {
                log_debug_printf(applog, "Forwarded beacon to %s -> %s -> %s?\n",
                                 msg.src.tostring().c_str(),
                                 msg.server.tostring().c_str(),
                                 std::string(SB()<<dest).c_str());
            }
        }
    }
};

} // namespace

int main(int argc, char *argv[])
{
    try {
        SockAttach attach;
        logger_config_env();
        App app;

        auto conf(server::Config::fromEnv());
        auto manager(UDPManager::instance());
        std::vector<std::unique_ptr<UDPListener>> listeners;

        auto onSearch = [&app](const UDPManager::Search& msg) {app.onSearch(msg);};
        auto onBeacon = [&app](const UDPManager::Beacon& msg) {app.onBeacon(msg);};

        {
            int opt;
            while ((opt = getopt(argc, argv, "L:F:phV")) != -1) {
                switch(opt) {
                case 'L':
                {
                    SockEndpoint ep(parseEP(optarg, conf));
                    listeners.push_back(manager.onSearch(ep, onSearch));
                    listeners.push_back(manager.onBeacon(ep, onBeacon));
                    break;
                }
                case 'F':
                    app.destinations.push_back(parseEP(optarg, conf));
                    break;
                case 'p':
                    conf.udp_port = parseTo<uint64_t>(optarg);
                    break;
                case 'h':
                    usage(argv[0]);
                    return 0;
                case 'V':
                    std::cout<<pvxs::version_information;
                    return 0;
                default:
                    usage(argv[0]);
                    std::cerr<<"\nError: Unknown argument: "<<char(opt)<<std::endl;
                    return 1;
                }
            }
        }

        if(argc!=optind) {
            usage(argv[0]);
            std::cerr<<"Error: Unexpected arguments."<<std::endl;
            return 1;
        }

        for(auto& listener : listeners) {
            listener->start();
        }

        epicsEvent done;
        SigInt H([&done]() {
            done.signal();
        });
        done.wait();

        return 0;
    }catch(std::exception& e){
        std::cerr<<"Error: "<<e.what()<<"\n";
        return 1;
    }
}
