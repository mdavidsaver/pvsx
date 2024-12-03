/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include "openssl.h"

#include <fstream>
#include <stdexcept>
#include <tuple>

#include <epicsExit.h>

#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>

#include <pvxs/log.h>

#include "certstatus.h"
#include "certstatusmanager.h"
#include "evhelper.h"
#include "ownedptr.h"
#include "serverconn.h"
#include "utilpvt.h"

#ifndef TLS1_3_VERSION
#error TLS 1.3 support required.  Upgrade to openssl >= 1.1.0
#endif

DEFINE_LOGGER(setup, "pvxs.ossl.init");
DEFINE_LOGGER(stapling, "pvxs.stapling");
DEFINE_LOGGER(watcher, "pvxs.certs.mon");
DEFINE_LOGGER(io, "pvxs.ossl.io");

namespace pvxs {
namespace ossl {

int ossl_verify(int preverify_ok, X509_STORE_CTX *x509_ctx) {
    X509 *cert_ptr = X509_STORE_CTX_get_current_cert(x509_ctx);
    if (preverify_ok) {
        // cert passed initial inspection, now check if revocation status is required
        if (!certs::CertStatusManager::statusMonitoringRequired(cert_ptr)) {
            return preverify_ok;  // No need to check status
        }

        // Status monitoring required, now check revocation status
        log_debug_println(watcher, "Current cert: %s\n", std::string(SB() << ShowX509{cert_ptr}).c_str());
        auto pva_ex_data = CertStatusExData::fromSSL_X509_STORE_CTX(x509_ctx);

        // Check if status monitoring is enabled
        // TODO Verify with working group that this logic is correct
        if (pva_ex_data->status_check_enabled) {
            auto peer_status = pva_ex_data->getCachedPeerStatus(cert_ptr);
            try {
                // Get status if current status is non existent or not valid
                if (!peer_status || !peer_status->isValid()) {
                    peer_status = pva_ex_data->setCachedPeerStatus(cert_ptr, certs::CertStatusManager::getStatus(ossl_ptr<X509>(X509_dup(cert_ptr))));
                }
                if (!peer_status->isGood()) {
                    return 0;  // At least one cert is not good
                }
            } catch (certs::CertStatusNoExtensionException &e) {
                log_err_printf(watcher, "Logic Error: Status monitored when not configured in cert: %s\n", std::string(SB() << ShowX509{cert_ptr}).c_str());
                exit(1);
            } catch (std::runtime_error &e) {
                log_warn_printf(watcher, "Unable to verify peer revocation status: %s\n", e.what());
                return 0;  // We need to verify the peer status but can't so fail
            }
        }
    } else {
        //        X509_STORE_CTX_print_verify_cb(preverify_ok, x509_ctx);
        auto err = X509_STORE_CTX_get_error(x509_ctx);

        // TODO Remove Dev mode to ignore contexts with no chain &
        // TODO Remove Dev mode to accept self signed certs as trusted
        // If the error is that the certificate is self-signed, we accept it
        if (err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY || err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
            err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN) {
            return preverify_ok;  // Accept self-signed certificates
        }
        log_err_printf(io, "Unable to verify peer cert: %s : %s\n", X509_verify_cert_error_string(err), std::string(SB() << ShowX509{cert_ptr}).c_str());
    }
    log_printf(io, preverify_ok ? Level::Debug : Level::Err, "TLS verify %s\n", preverify_ok ? "Ok" : "Reject");
    return preverify_ok;
}

namespace {

constexpr int ossl_verify_depth = 5;

// see NOTE in "man SSL_CTX_set_alpn_protos"
const unsigned char pva_alpn[] = "\x05pva/1";

struct OSSLGbl {
    ossl_ptr<OSSL_LIB_CTX> libctx;
    int SSL_CTX_ex_idx;
#ifdef PVXS_ENABLE_SSLKEYLOGFILE
    std::ofstream keylog;
    epicsMutex keylock;
#endif
} *ossl_gbl;

#ifdef PVXS_ENABLE_SSLKEYLOGFILE
void sslkeylogfile_exit(void *) noexcept {
    auto gbl = ossl_gbl;
    try {
        epicsGuard<epicsMutex> G(gbl->keylock);
        if (gbl->keylog.is_open()) {
            gbl->keylog.flush();
            gbl->keylog.close();
        }
    } catch (std::exception &e) {
        static bool once = false;
        if (!once) {
            fprintf(stderr, "Error while writing to SSLKEYLOGFILE\n");
            once = true;
        }
    }
}

void sslkeylogfile_log(const SSL *, const char *line) noexcept {
    auto gbl = ossl_gbl;
    try {
        epicsGuard<epicsMutex> G(gbl->keylock);
        if (gbl->keylog.is_open()) {
            gbl->keylog << line << '\n';
            gbl->keylog.flush();
        }
    } catch (std::exception &e) {
        static bool once = false;
        if (!once) {
            fprintf(stderr, "Error while writing to SSLKEYLOGFILE\n");
            once = true;
        }
    }
}
#endif  // PVXS_ENABLE_SSLKEYLOGFILE

void free_SSL_CTX_sidecar(void *, void *ptr, CRYPTO_EX_DATA *, int , long , void *) noexcept {
    auto car = static_cast<CertStatusExData *>(ptr);
    delete car;
}

void OSSLGbl_init() {
    ossl_ptr<OSSL_LIB_CTX> ctx(__FILE__, __LINE__, OSSL_LIB_CTX_new());
    // read $OPENSSL_CONF or eg. /usr/lib/ssl/openssl.cnf
    (void)CONF_modules_load_file_ex(ctx.get(), NULL, "pvxs", CONF_MFLAGS_IGNORE_MISSING_FILE | CONF_MFLAGS_IGNORE_RETURN_CODES);
    std::unique_ptr<OSSLGbl> gbl{new OSSLGbl};
    gbl->SSL_CTX_ex_idx = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, free_SSL_CTX_sidecar);
#ifdef PVXS_ENABLE_SSLKEYLOGFILE
    if (auto env = getenv("SSLKEYLOGFILE")) {
        epicsGuard<epicsMutex> G(gbl->keylock);
        gbl->keylog.open(env);
        if (gbl->keylog.is_open()) {
            epicsAtExit(sslkeylogfile_exit, nullptr);
            log_warn_printf(setup, "TLS Debug Enabled: logging TLS secrets to %s\n", env);
        } else {
            log_err_printf(setup, "TLS Debug Disabled: Unable to open SSL key log file: %s\n", env);
        }
    }
#endif  // PVXS_ENABLE_SSLKEYLOGFILE
    ossl_gbl = gbl.release();
}

int ossl_alpn_select(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *) {
    unsigned char *selected;
    auto ret(SSL_select_next_proto(&selected, outlen, pva_alpn, sizeof(pva_alpn) - 1u, in, inlen));
    if (ret == OPENSSL_NPN_NEGOTIATED) {
        *out = selected;
        log_debug_printf(io, "TLS ALPN select%s", "\n");
        return SSL_TLSEXT_ERR_OK;
    } else {  // OPENSSL_NPN_NO_OVERLAP
        log_err_printf(io, "TLS ALPN reject%s", "\n");
        return SSL_TLSEXT_ERR_ALERT_FATAL;  // could fail soft w/ SSL_TLSEXT_ERR_NOACK
    }
}

/**
 * @brief Verifies the key usage of a given certificate.
 *
 * This function checks the key usage extension of the specified certificate
 * and verifies that the key usage flags match the intended purpose.
 * If ssl_client is set to true, it will verify that the key usage includes
 * the key encipherment flag.
 *
 * If ssl_client is set to false, it will verify
 * that the key usage includes the digital signature flag.
 *
 * @param cert The X509 certificate to verify key usage for.
 * @param ssl_client A flag indicating whether the certificate is for SSL
 * client.
 * @return Don't throw if the key usage is valid for the intended purpose,
 * throw an exception otherwise.
 */
void verifyKeyUsage(const ossl_ptr<X509> &cert,
                    bool ssl_client) {  // some early sanity checks
    auto flags(X509_get_extension_flags(cert.get()));
    auto kusage(X509_get_extended_key_usage(cert.get()));

    if (flags & EXFLAG_CA) throw std::runtime_error(SB() << "Found CA Certificate when End Entity expected");

    if ((ssl_client && !(kusage & XKU_SSL_CLIENT)) || (!ssl_client && !(kusage & XKU_SSL_SERVER)))
        throw std::runtime_error(SB() << "Extended Key Usage does not permit usage as a Secure PVAccesss " << (ssl_client ? "Client" : "Server"));

    log_debug_printf(setup, "Using%s cert %s\n", (flags & EXFLAG_SS) ? " self-signed" : "", std::string(SB() << ShowX509{cert.get()}).c_str());
}

/**
 * @brief Extracts the certificate authorities from the provided CAs and
 * adds them to the given context.
 *
 * java keytool adds an extra attribute to indicate that a certificate
 * is trusted.  However, PKCS12_parse() circa 3.1 does not know about
 * this, and gives us all the certs. in one blob for us to sort through.
 *
 * We _assume_ that any root CA included in a PKCS#12 file is meant to
 * be trusted.  Otherwise, such a cert. could never appear in a valid
 * chain.
 *
 * @param ctx the context to add the CAs to
 * @param CAs the stack of X509 CA certificates
 */
void extractCAs(SSLContext &ctx, const ossl_ptr<stack_st_X509> &CAs) {
    for (int i = 0, N = sk_X509_num(CAs.get()); i < N; i++) {
        auto ca = sk_X509_value(CAs.get(), i);

        auto canSign(X509_check_ca(ca));
        auto flags(X509_get_extension_flags(ca));

        if (canSign == 0 && i != 0) {
            log_err_printf(setup, "non-CA certificate in PKCS#12 chain%s\n", "");
            log_err_printf(setup, "%s\n", (SB() << ShowX509{ca}).str().c_str());
            throw std::runtime_error(SB() << "non-CA certificate found in PKCS#12 chain");
        }

        if (flags & EXFLAG_SS) {        // self-signed (aka. root)
            assert(flags & EXFLAG_SI);  // circa OpenSSL, self-signed implies self-issued

            log_debug_println(setup, "Trusting root CA %s\n", std::string(SB() << ShowX509{ca}).c_str());

            // populate the context's trust store with the root cert
            X509_STORE *trusted_store = SSL_CTX_get_cert_store(ctx.ctx);
            if (!X509_STORE_add_cert(trusted_store, ca)) throw SSLError("X509_STORE_add_cert");
        } else {  // signed by another CA
            log_debug_println(setup, "Using untrusted/chain CA cert %s\n", std::string(SB() << ShowX509{ca}).c_str());
            // note: chain certs added this way are ignored unless SSL_BUILD_CHAIN_FLAG_UNTRUSTED is used
            // appends SSL_CTX::cert::chain
        }
        if (!SSL_CTX_add0_chain_cert(ctx.ctx, ca)) throw SSLError("SSL_CTX_add0_chain_cert");

        // TODO monitor this certificate status and disableTLS if becomes invalid and only continue if the status is good
    }
}

/**
 * @brief Get a certificate from the given file
 *
 * This function reads a p12 object from the provided file and returns true if successful.
 *
 * @param fp The file pointer of the p12 file.
 * @param p12 The PKCS12 object reference to be updated
 * @param ssl_client True if the request came from a client, false from a server
 * @param cert_filename The filename of the p12 file
 *
 * @return true if the p12 file exists and can be read
 * @return false if the p12 file does not exist or is invalid and the request was from a client
 *
 * @throws std::runtime_error("Invalid, Untrusted, or Nonexistent cert file  ...")
 *         if p12 file not found or invalid and it if for a server
 */
bool checkP12File(file_ptr &fp, ossl_ptr<PKCS12> &p12, const uint16_t &usage, const std::string &cert_filename) {
    // Return true if it exists and is readable
    if (fp && d2i_PKCS12_fp(fp.get(), p12.acquire())) return true;

    // If file not found or unreadable
    if (usage == ssl::kForClient) {
        // Client ONLY can create SSL session even though it has no certificate/key as long as the server allows it
        return false;
    } else {
        throw std::runtime_error(SB() << "Invalid, Untrusted, or Nonexistent cert file at [" << cert_filename << "]");
    }
}

/**
 * @brief Get the key and certificate from the given p12 file
 *
 * @param filename - The filename of the p12 file
 * @param password - The password for the p12 file
 * @param ssl_client - True if the request came from a client, false from a server
 * @param key - The key to be retrieved
 * @param cert - The certificate to be retrieved
 * @param CAs - The CA certificates to be retrieved
 * @param get_key - True if the key should be retrieved
 * @param get_cert - True if the certificate and chain should be retrieved
 * @return True if the key and certificate were retrieved successfully
 * @throws SSLError if the p12 file can't be processed
 */
bool getKeyAndCertFromP12File(const std::string filename, const std::string password, bool ssl_client, ossl_ptr<EVP_PKEY> &key, ossl_ptr<X509> &cert,
                              ossl_ptr<STACK_OF(X509)> &CAs, bool get_key = true, bool get_cert = true) {
    log_debug_printf(setup, "PKCS12 filename %s;%s\n", filename.c_str(), password.empty() ? "" : " w/ password");

    // Open the p12 file
    file_ptr fp(fopen(filename.c_str(), "rb"), false);

    // Check if the p12 file is valid
    ossl_ptr<PKCS12> p12;
    if (!checkP12File(fp, p12, (ssl_client ? ssl::kForClient : ssl::kForServer), filename)) return false;

    // If both key and certificate are to be retrieved
    if (get_key && get_cert) {
        if (PKCS12_parse(p12.get(), password.c_str(), key.acquire(), cert.acquire(), CAs.acquire())) return true;
    } else if (get_key) {
        // If only the key is to be retrieved
        if (PKCS12_parse(p12.get(), password.c_str(), key.acquire(), nullptr, nullptr)) return true;
    } else {
        // If only the certificate and chain are to be retrieved
        ossl_ptr<EVP_PKEY> pkey;  // to discard
        if (PKCS12_parse(p12.get(), password.c_str(), pkey.acquire(), cert.acquire(), CAs.acquire())) return true;
    }

    // If the p12 file can't be processed, throw an error
    throw SSLError(SB() << "Unable to process \"" << filename << "\"");
}

/**
 * @brief Get the key from the given p12 file
 *
 * @param filename - The filename of the p12 file
 * @param password - The password for the p12 file
 * @param ssl_client - True if the request came from a client, false from a server
 * @param key - The key to be retrieved
 * @return True if the key was retrieved successfully
 * @throws SSLError if the p12 file can't be processed
 */
bool getKeyFromP12File(const std::string filename, const std::string password, bool ssl_client, ossl_ptr<EVP_PKEY> &key) {
    ossl_ptr<X509> cert;
    ossl_ptr<STACK_OF(X509)> CAs(__FILE__, __LINE__, sk_X509_new_null());
    return getKeyAndCertFromP12File(filename, password, ssl_client, key, cert, CAs, true, false);
}

/**
 * @brief Get the certificate and chain from the given p12 file
 *
 * @param filename - The filename of the p12 file
 * @param password - The password for the p12 file
 * @param ssl_client - True if the request came from a client, false from a server
 * @param cert - The certificate to be retrieved
 * @param CAs - The CA certificates to be retrieved
 * @return True if the certificate was retrieved successfully
 * @throws SSLError if the p12 file can't be processed
 */
bool getCertFromP12File(const std::string filename, const std::string password, bool ssl_client, ossl_ptr<X509> &cert, ossl_ptr<STACK_OF(X509)> &CAs) {
    ossl_ptr<EVP_PKEY> key;
    return getKeyAndCertFromP12File(filename, password, ssl_client, key, cert, CAs, false, true);
}

/**
 * @brief Common setup for OpenSSL SSL context
 *
 * This function sets up the OpenSSL SSL context used for SSL/TLS communication.
 * It configures the SSL method, whether it is for a client or a server, and the
 * common configuration options.
 *
 * @param method The SSL_METHOD object representing the SSL method to use.
 * @param ssl_client A boolean indicating whether the setup is for a client or a
 * server.
 * @param conf The common configuration object.
 *
 * @return SSLContext initialised appropriately - clients can have an empty
 * context so that they can connect to ssl servers without having a certificate
 */
SSLContext ossl_setup_common(const SSL_METHOD *method, bool ssl_client, const impl::ConfigCommon &conf) {
    impl::threadOnce<&OSSLGbl_init>();

    // Initialise SSL subsystem and add our custom extensions (idempotent)
    SSLContext::sslInit();

    SSLContext tls_context;
    tls_context.status_check_disabled = conf.tls_disable_status_check;
    tls_context.stapling_disabled = conf.tls_disable_stapling;
    tls_context.ctx = SSL_CTX_new_ex(ossl_gbl->libctx.get(), NULL, method);
    if (!tls_context.ctx) throw SSLError("Unable to allocate SSL_CTX");

    {
        std::unique_ptr<CertStatusExData> car{new CertStatusExData(!conf.tls_disable_status_check)};
        if (!SSL_CTX_set_ex_data(tls_context.ctx, ossl_gbl->SSL_CTX_ex_idx, car.get())) throw SSLError("SSL_CTX_set_ex_data");
        car.release();  // SSL_CTX_free() now responsible
    }

#ifdef PVXS_ENABLE_SSLKEYLOGFILE
    //    assert(!SSL_CTX_get_keylog_callback(ctx.ctx));
    (void)SSL_CTX_set_keylog_callback(tls_context.ctx, &sslkeylogfile_log);
#endif

    // TODO: SSL_CTX_set_options(), SSL_CTX_set_mode() ?

    // we mandate TLS >= 1.3
    (void)SSL_CTX_set_min_proto_version(tls_context.ctx, TLS1_3_VERSION);
    (void)SSL_CTX_set_max_proto_version(tls_context.ctx, 0);  // up to max.

    if (ssl_client && conf.tls_disabled) {
        // For clients if tls is disabled then allow server to make a tls
        // connection if it can but disable client side
        return tls_context;
    }

    if (conf.isTlsConfigured()) {
        const std::string &filename = conf.tls_cert_filename, &password = conf.tls_cert_password;
        auto key_filename = conf.tls_private_key_filename.empty() ? filename : conf.tls_private_key_filename;
        auto key_password = conf.tls_private_key_password.empty() ? password : conf.tls_private_key_password;

        ossl_ptr<EVP_PKEY> key;
        ossl_ptr<X509> cert;
        ossl_ptr<STACK_OF(X509)> CAs(__FILE__, __LINE__, sk_X509_new_null());

        // get the key and certificate from the p12 file or files
        if (key_filename == filename) {
            if (!getKeyAndCertFromP12File(filename, password, ssl_client, key, cert, CAs)) return tls_context;
        } else {
            if (!getKeyFromP12File(key_filename, key_password, ssl_client, key)) return tls_context;
            if (!getCertFromP12File(filename, password, ssl_client, cert, CAs)) return tls_context;
        }

        if (cert) {
            // some early sanity checks
            verifyKeyUsage(cert, ssl_client);
        }

        // sets SSL_CTX::cert
        if (cert && !SSL_CTX_use_certificate(tls_context.ctx, cert.get())) throw SSLError("SSL_CTX_use_certificate");
        if (key && !SSL_CTX_use_PrivateKey(tls_context.ctx, key.get())) throw SSLError("SSL_CTX_use_certificate");

        // extract CAs (intermediate and root) from PKCS12 bag
        extractCAs(tls_context, CAs);

        if (key && !SSL_CTX_check_private_key(tls_context.ctx)) throw SSLError("invalid private key");

        // Move cert to the context
        if (cert) {
            auto ex_data = tls_context.ex_data();
            ex_data->cert = std::move(cert);
            tls_context.has_cert = true;

            // Build the certificate chain and set verification flags
            if (!SSL_CTX_build_cert_chain(tls_context.ctx, SSL_BUILD_CHAIN_FLAG_CHECK))  // Check build chain
                // if (!SSL_CTX_build_cert_chain(tls_context.ctx, SSL_BUILD_CHAIN_FLAG_UNTRUSTED))  // Flag untrusted in build chain
                // if (!SSL_CTX_build_cert_chain(tls_context.ctx, 0))  // checks default operation
                throw SSLError("invalid cert chain");

            // If status check is disabled, set the certificate as valid immediately
            if (tls_context.status_check_disabled) {
                tls_context.cert_is_valid = true;
            }
        }
    }

    {
        /* wrt. SSL_VERIFY_CLIENT_ONCE
         *   TLS 1.3 does not support session renegotiation.
         *   Does allow server to re-request client cert. via CertificateRequest.
         *   However, no way for client to re-request server cert.
         *   So we don't bother with this, and instead for connection reset
         *   when new certs. loaded.
         */
        int mode = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
        if (!ssl_client && conf.tls_client_cert_required == ConfigCommon::Require) {
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            log_debug_printf(setup, "This Secure PVAccess Server requires an X.509 client certificate%s", "\n");
        }
        SSL_CTX_set_verify(tls_context.ctx, mode, &ossl_verify);
        SSL_CTX_set_verify_depth(tls_context.ctx, ossl_verify_depth);
    }
    return tls_context;
}

}  // namespace

/**
 * @brief This is the callback that is made by the TLS handshake to add the server OCSP status to the payload
 *
 * @param tls_context the tls context to add the OCSP response to
 */
int serverOCSPCallback(SSL *ssl, pvxs::server::Server::Pvt *server) {
    if (SSL_get_tlsext_status_type(ssl) != -1) {
        // Should never be triggered.  Because the callback should only be called when the client has requested stapling.
        return SSL_TLSEXT_ERR_ALERT_WARNING;
    }

    if (!server->current_status) {
        log_warn_printf(stapling, "Server OCSP Stapling: No server status to staple%s\n", "");
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    auto ocsp_data_ptr = (void *)server->current_status->ocsp_bytes.data();
    auto ocsp_data_len = server->current_status->ocsp_bytes.size();

    if (!server->cached_ocsp_response || memcmp(ocsp_data_ptr, server->cached_ocsp_response, ocsp_data_len)) {
        // if status has changed
        if (server->cached_ocsp_response) {
            OPENSSL_free(server->cached_ocsp_response);
        }
        server->cached_ocsp_response = OPENSSL_malloc(ocsp_data_len);
        memcpy(server->cached_ocsp_response, ocsp_data_ptr, ocsp_data_len);

        if (SSL_set_tlsext_status_ocsp_resp(ssl, server->cached_ocsp_response, ocsp_data_len) != 1) {
            log_warn_printf(stapling, "Server OCSP Stapling: unable to staple server status%s\n", "");
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        } else
            log_info_printf(stapling, "Server OCSP Stapling: server status stapled%s\n", "");
    }
    return SSL_TLSEXT_ERR_OK;
}

/**
 * @brief Staple server's ocsp response to the tls handshake
 * @param ssl the ssl context
 * @param arg
 * @return
 */
void stapleOcspResponse(void *server_ptr, SSL *) {
    auto server = (pvxs::server::Server::Pvt *)server_ptr;
    SSL_CTX_set_tlsext_status_cb(server->tls_context.ctx, serverOCSPCallback);
    SSL_CTX_set_tlsext_status_arg(server->tls_context.ctx, server);
}

// Must be set up with correct values after OpenSSL initialisation to retrieve status PV from certs
int SSLContext::NID_PvaCertStatusURI = NID_undef;

/**
 * @brief Sets the peer status for the given serial number
 * @param serial_number - Serial number
 * @param status - Certificate status
 * @return The peer status that was set
 */
std::shared_ptr<const certs::CertificateStatus> CertStatusExData::setCachedPeerStatus(serial_number_t serial_number, const certs::CertificateStatus &status) {
    return setCachedPeerStatus(serial_number, std::make_shared<certs::CertificateStatus>(status));
}

/**
 * @brief Subscribes to cert status if required and not already monitoring
 * @param cert_ptr - Certificate status to subscribe to
 * @param fn - Function to call when the certificate status changes from good to bad or vice versa
 */
void CertStatusExData::subscribeToCertStatus(X509 *cert_ptr, std::function<void(bool)> fn) {
    auto serial_number = getSerialNumber(cert_ptr);
    auto &cert_status_manager = peer_statuses[serial_number].cert_status_manager;

    if (cert_status_manager) return;  // Already subscribed

    try {
        // Duplicate the certificate
        auto cert_to_monitor = ossl_ptr<X509>(X509_dup(cert_ptr));
        // Subscribe to the certificate status
        cert_status_manager = certs::CertStatusManager::subscribe(std::move(cert_to_monitor), [=](certs::PVACertificateStatus status) {
            Guard G(lock);
            // Get the previous status
            auto previous_status = getCachedPeerStatus(serial_number);
            // Check if the previous status was good
            auto was_good = previous_status && previous_status->isGood();
            // Get the current state while setting the cached peer status
            auto current_status = setCachedPeerStatus(serial_number, status);
            // Check if the current status is good
            bool is_good = current_status && current_status->isGood();
            UnGuard U(G);
            // If the state has changed, call the function
            if (is_good != was_good) {
                fn(is_good);
            }
        });
    } catch (...) {
    }
}

CertStatusExData *CertStatusExData::fromSSL_X509_STORE_CTX(X509_STORE_CTX *x509_ctx) {
    SSL *ssl = (SSL *)X509_STORE_CTX_get_ex_data(x509_ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    return fromSSL(ssl);
}

CertStatusExData *CertStatusExData::fromSSL(SSL *ssl) {
    if (!ssl) {
        return nullptr;
    }
    SSL_CTX *ssl_ctx = SSL_get_SSL_CTX(ssl);
    return fromSSL_CTX(ssl_ctx);
}

CertStatusExData *CertStatusExData::fromSSL_CTX(SSL_CTX *ssl_ctx) {
    if (!ssl_ctx) {
        return nullptr;
    }
    return static_cast<CertStatusExData *>(SSL_CTX_get_ex_data(ssl_ctx, ossl_gbl->SSL_CTX_ex_idx));
}

CertStatusExData *SSLContext::ex_data() const { return CertStatusExData::fromSSL_CTX(ctx); }

const X509 *SSLContext::certificate0() const {
    if (!ctx) throw std::invalid_argument("NULL");

    auto car = static_cast<CertStatusExData *>(SSL_CTX_get_ex_data(ctx, ossl_gbl->SSL_CTX_ex_idx));
    return car->cert.get();
}

bool SSLContext::fill_credentials(PeerCredentials &C, const SSL *ctx) {
    if (!ctx) throw std::invalid_argument("NULL");

    if (auto cert = SSL_get0_peer_certificate(ctx)) {
        PeerCredentials temp(C);  // copy current as initial (don't overwrite isTLS)
        auto subj = X509_get_subject_name(cert);
        char name[64];
        if (subj && X509_NAME_get_text_by_NID(subj, NID_commonName, name, sizeof(name) - 1)) {
            name[sizeof(name) - 1] = '\0';
            log_debug_printf(io, "Peer CN=%s\n", name);
            temp.method = "x509";
            temp.account = name;

            // try to use root CA name to qualify authority
            if (auto chain = SSL_get0_verified_chain(ctx)) {
                auto N = sk_X509_num(chain);
                X509 *root;
                X509_NAME *rootName;
                // last cert should be root CA
                if (N && !!(root = sk_X509_value(chain, N - 1)) && !!(rootName = X509_get_subject_name(root)) &&
                    X509_NAME_get_text_by_NID(rootName, NID_commonName, name, sizeof(name) - 1)) {
                    if (X509_check_ca(root) && (X509_get_extension_flags(root) & EXFLAG_SS)) {
                        temp.authority = name;

                    } else {
                        log_warn_printf(io, "Last cert in peer chain is not root CA?!? %s\n", std::string(SB() << ossl::ShowX509{root}).c_str());
                    }
                }
            }
        }

        C = std::move(temp);
        return true;
    } else {
        return false;
    }
}

SSLContext SSLContext::for_client(const impl::ConfigCommon &conf) {
    auto ctx(ossl_setup_common(TLS_client_method(), true, conf));

    if (0 != SSL_CTX_set_alpn_protos(ctx.ctx, pva_alpn, sizeof(pva_alpn) - 1))
        throw SSLError("Unable to agree on Application Layer Protocol to use: Both sides should use pva/1");

    return ctx;
}

SSLContext SSLContext::for_server(const impl::ConfigCommon &conf) {
    auto ctx(ossl_setup_common(TLS_server_method(), false, conf));

    SSL_CTX_set_alpn_select_cb(ctx.ctx, &ossl_alpn_select, nullptr);

    return ctx;
}

SSLError::SSLError(const std::string &msg)
    : std::runtime_error([&msg]() -> std::string {
          std::ostringstream strm;
          const char *file = nullptr;
          int line = 0;
          const char *data = nullptr;
          int flags = 0;
          while (auto err = ERR_get_error_all(&file, &line, nullptr, &data, &flags)) {
              strm << file << ':' << line << ':' << ERR_reason_error_string(err);
              if (data && (flags & ERR_TXT_STRING)) strm << ':' << data;
              strm << ", ";
          }
          strm << msg;
          return strm.str();
      }()) {}

SSLError::~SSLError() = default;

std::ostream &operator<<(std::ostream &strm, const ShowX509 &cert) {
    if (cert.cert) {
        auto name = X509_get_subject_name(cert.cert);
        auto issuer = X509_get_issuer_name(cert.cert);
        assert(name);
        ossl_ptr<BIO> io(__FILE__, __LINE__, BIO_new(BIO_s_mem()));
        (void)BIO_printf(io.get(), "subject:");
        (void)X509_NAME_print(io.get(), name, 1024);
        (void)BIO_printf(io.get(), " issuer:");
        (void)X509_NAME_print(io.get(), issuer, 1024);
        if (auto atm = X509_get0_notBefore(cert.cert)) {
            (void)BIO_printf(io.get(), " from: ");
            ASN1_TIME_print(io.get(), atm);
        }
        if (auto atm = X509_get0_notAfter(cert.cert)) {
            (void)BIO_printf(io.get(), " until: ");
            ASN1_TIME_print(io.get(), atm);
        }
        {
            char *str = nullptr;
            if (auto len = BIO_get_mem_data(io.get(), &str)) {
                strm.write(str, len);
            }
        }
    } else {
        strm << "NULL";
    }
    return strm;
}

}  // namespace ossl
}  // namespace pvxs
