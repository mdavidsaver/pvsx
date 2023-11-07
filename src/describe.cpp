/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#if !defined(_WIN32)
#  include <sys/utsname.h>
#endif

#include <ostream>
#include <string>

#include <osiSock.h>
#include <dbDefs.h>
#include <epicsThread.h>

#include <pvxs/util.h>
#include <pvxs/client.h>
#include <pvxs/server.h>

#include "evhelper.h"
#include "describe.h"

namespace pvxs {

std::ostream& target_information(std::ostream& strm)
{
    strm<<indent{}<<"Host: "<<EPICS_HOST_ARCH<<"\n";
    strm<<indent{}<<"Target: "<<T_A<<" "<<OS_CLASS;
#if EPICS_VERSION_INT>=VERSION_INT(3,15,0,1)
    strm<<" "<<CMPLR_CLASS;
#endif
    strm<<"\n";

    {
        strm<<indent{}<<"Toolchain\n";
        Indented I(strm);

        // standard
        strm<<indent{}<<"__cplusplus = "<<__cplusplus<<"\n";

        // compiler
#ifdef __clang__
        strm<<indent{}<<"clang "<<__clang_version__<<"\n";
#endif
#ifdef __GNUC__
        strm<<indent{}<<"GCC "<<__GNUC__<<"."<<__GNUC_MINOR__<<"."<<__GNUC_PATCHLEVEL__<<"\n";
#endif
#ifdef _GLIBCXX_USE_CXX11_ABI
        strm<<indent{}<<"_GLIBCXX_USE_CXX11_ABI = "<<_GLIBCXX_USE_CXX11_ABI<<"\n";
#endif
#ifdef _MSC_VER
        strm<<indent{}<<"MSVC "<<_MSC_FULL_VER<<"\n";
#endif

        // library
#ifdef __GLIBC__
        strm<<indent{}<<"GLIBC "<<__GLIBC__<<"."<<__GLIBC_MINOR__<<"\n";
#endif
#ifdef __UCLIBC__
        strm<<indent{}<<"GLIBC "<<__UCLIBC_MAJOR__<<"."<<__UCLIBC_MINOR__<<"."<<__UCLIBC_SUBLEVEL__<<"\n";
#endif
#ifdef _CPPLIB_VER
        // Dinkumware c++
        strm<<indent{}<<"_CPPLIB_VER "<<_CPPLIB_VER<<"\n";
#endif
#ifdef __GLIBCXX__
        strm<<indent{}<<"__GLIBCXX__ "<<__GLIBCXX__<<"\n";
#endif
#ifdef _LIBCPP_VERSION
        // clang c++
        strm<<indent{}<<"_LIBCPP_VERSION "<<_LIBCPP_VERSION<<"\n";
#endif
    }

    {
        strm<<indent{}<<"Versions\n";
        Indented I(strm);
        strm<<indent{}<<version_str()<<"\n";
        strm<<indent{}<<EPICS_VERSION_STRING<<"\n";
        strm<<indent{}<<"libevent "<<event_get_version()<<"\n";
    }

    {
        strm<<indent{}<<"Runtime\n";
        Indented I(strm);

        SockAttach attach;
        evsocket dummy(AF_INET, SOCK_DGRAM, 0);

#if !defined(_WIN32)
        utsname info;
        if(uname(&info)==0) {
            strm<<indent{}<<"uname() -> "<<info.sysname<<" "<<info.nodename<<" "<<info.release<<" "<<info.version<<" "<<info.machine<<"\n";
        } else {
            strm<<indent{}<<"uname() error "<<errno<<"\n";
        }
#endif

#if EPICS_VERSION_INT>=VERSION_INT(3,15,0,2)
        strm<<indent{}<<"epicsThreadGetCPUs() -> "<<epicsThreadGetCPUs()<<"\n";
#endif

        auto localaddr(osiLocalAddr(dummy.sock));
        strm<<indent{}<<"osiLocalAddr() -> "<<SockAddr(&localaddr.sa).tostring()<<"\n";

        strm<<indent{}<<"osiSockDiscoverBroadcastAddresses() ->\n";
        Indented J(strm);
        for(auto& addr : dummy.broadcasts()) {
            strm<<indent{}<<addr.tostring()<<"\n";
        }
    }

    {
        strm<<indent{}<<"Effective Client config from environment\n";
        Indented I(strm);
        auto conf(client::Config::fromEnv());
        conf.expand();
        strm<<conf;
    }

    {
        strm<<indent{}<<"Effective Server config from environment\n";
        Indented I(strm);
        auto conf(server::Config::fromEnv());
        conf.expand();
        strm<<conf;
    }

    strm.flush();
    return strm;
}

std::ostream& version_information(std::ostream& strm)
{
    strm<<version_str()<<"\n";
    strm<<EPICS_VERSION_STRING<<"\n";
    strm<<"libevent "<<event_get_version()<<"\n";
#ifdef PVXS_ENABLE_OPENSSL
    strm<<OPENSSL_VERSION_TEXT<<"\n";
#else
    strm<<"OpenSSL not enabled\n";
#endif
    return strm;
}

} // namespace pvxs
