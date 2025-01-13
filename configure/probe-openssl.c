
#include <openssl/opensslv.h>

#ifndef OPENSSL_VERSION_NUMBER
#  error Some antique OpenSSL version?
#endif
#if OPENSSL_VERSION_NUMBER < 0x30000000
#  error Minimum OpenSSL 3.0
#endif

#include <event2/event-config.h>

#ifndef EVENT__HAVE_OPENSSL
#  error libevent not built with OpenSSL support
#endif

#include <event2/bufferevent_ssl.h>
