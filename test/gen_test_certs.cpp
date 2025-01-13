/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <openssl/stack.h>
#include <openssl/err.h>

#include <epicsGetopt.h>

namespace {
// cleanup hooks for use with std::unique_ptr
template<typename T>
struct ssl_delete;
#define DEFINE_DELETE(TYPE) \
    template<> \
    struct ssl_delete<TYPE> { \
        inline void operator()(TYPE* fp) { if(fp) TYPE ## _free(fp); } \
    }
DEFINE_DELETE(BIO);
DEFINE_DELETE(ASN1_OBJECT);
DEFINE_DELETE(ASN1_INTEGER);
static_assert(std::is_same<ASN1_INTEGER, ASN1_TIME>::value, "");
static_assert(std::is_same<ASN1_INTEGER, ASN1_OCTET_STRING>::value, "");
DEFINE_DELETE(AUTHORITY_KEYID);
DEFINE_DELETE(BASIC_CONSTRAINTS);
DEFINE_DELETE(PKCS12);
DEFINE_DELETE(EVP_PKEY_CTX);
DEFINE_DELETE(EVP_PKEY);
DEFINE_DELETE(X509);
DEFINE_DELETE(X509_NAME);
DEFINE_DELETE(X509_PUBKEY);
DEFINE_DELETE(X509_EXTENSION);
DEFINE_DELETE(X509_ATTRIBUTE);
DEFINE_DELETE(GENERAL_NAME);
DEFINE_DELETE(GENERAL_NAMES);
template<>
struct ssl_delete<FILE> {
    inline void operator()(FILE* fp) { if(fp) fclose(fp); }
};
#define DEFINE_SK_DELETE(TYPE) \
    template<> \
    struct ssl_delete<STACK_OF(TYPE)> { \
        inline void operator()(STACK_OF(TYPE)* fp) { if(fp) sk_ ## TYPE ## _free(fp); } \
    }
DEFINE_SK_DELETE(X509);
DEFINE_SK_DELETE(X509_ATTRIBUTE);
#undef DEFINE_DELETE

struct SSLError : public std::runtime_error {
    explicit
    SSLError(const std::string& msg)
        :std::runtime_error([&msg]() -> std::string {
            std::ostringstream strm;
            const char *file = nullptr;
            int line = 0;
            const char *data = nullptr;
            int flags = 0;
            while(auto err = ERR_get_error_all(&file, &line, nullptr, &data, &flags)) {
                strm<<file<<':'<<line<<':'<<ERR_reason_error_string(err);
                if(data && (flags&ERR_TXT_STRING))
                    strm<<':'<<data;
                strm<<", ";
            }
            strm<<msg;
            return strm.str();
    }())
    {}
    virtual ~SSLError() {}
};

struct SB {
    std::ostringstream strm;
    SB() {}
    operator std::string() const { return strm.str(); }
    std::string str() const { return strm.str(); }
    template<typename T>
    SB& operator<<(const T& i) { strm<<i; return *this; }
};

// ~= std::unique_ptr with a NULL check in the ctor
template<typename T>
struct owned_ptr : public std::unique_ptr<T, ssl_delete<T>>
{
    constexpr owned_ptr() {}
    constexpr owned_ptr(std::nullptr_t np) : std::unique_ptr<T, ssl_delete<T>>(np) {}
    explicit owned_ptr(T* ptr) : std::unique_ptr<T, ssl_delete<T>>(ptr) {
        if(!*this)
            throw SSLError(SB()<<"Can't alloc "<<typeid(ptr).name());
    }

    // for functions which return a pointer in an argument
    //   int some(T** presult); // store *presult = output
    // use like
    //   owned_ptr<T> x;
    //   some(x.acquire());
    struct acquisition {
        owned_ptr<T>* o;
        T* ptr = nullptr;
        operator T** () { return &ptr; }
        constexpr acquisition(owned_ptr<T>* o) :o(o) {}
        ~acquisition() {
            o->reset(ptr);
        }
    };
    acquisition acquire() { return acquisition{this}; }
};

// many openssl calls return 1 (or sometimes zero) on success.
void _must_equal(int expect, int actual, const char *expr)
{
    if(expect!=actual)
        throw SSLError(SB()<<expect<<"!="<<actual<<" : "<<expr);
}
#define _STR(STR) #STR
#define MUST(EXPECT, ...) _must_equal(EXPECT, __VA_ARGS__, _STR(__VA_ARGS__))

#ifdef NID_oracle_jdk_trustedkeyusage
// OpenSSL 3.2 will add the ability to set the Java specific trustedkeyusage bag attribute
static int jdk_trust(PKCS12_SAFEBAG *bag, void *cbarg) noexcept {
    try {
        // Only add trustedkeyusage when bag is an X509 cert. with an associated key
        // (when localKeyID is present) which does not already have trustedkeyusage.
        if(PKCS12_SAFEBAG_get_nid(bag)!=NID_certBag
                || PKCS12_SAFEBAG_get_bag_nid(bag)!=NID_x509Certificate
                || !!PKCS12_SAFEBAG_get0_attr(bag, NID_localKeyID)
                || !!PKCS12_SAFEBAG_get0_attr(bag, NID_oracle_jdk_trustedkeyusage))
            return 1;

        auto curattrs(PKCS12_SAFEBAG_get0_attrs(bag));
        // PKCS12_SAFEBAG_get0_attrs() returns const.  Make a paranoia copy.
        owned_ptr<STACK_OF(X509_ATTRIBUTE)> newattrs(sk_X509_ATTRIBUTE_deep_copy(curattrs,
                                                                                 &X509_ATTRIBUTE_dup,
                                                                                 &X509_ATTRIBUTE_free));

        owned_ptr<ASN1_OBJECT> trust(OBJ_txt2obj("anyExtendedKeyUsage", 0));
        owned_ptr<X509_ATTRIBUTE> attr(X509_ATTRIBUTE_create(NID_oracle_jdk_trustedkeyusage,
                                                             V_ASN1_OBJECT, trust.get()));

        MUST(1, sk_X509_ATTRIBUTE_push(newattrs.get(), attr.get()));
        attr.release();

        PKCS12_SAFEBAG_set0_attrs(bag, newattrs.get());
        newattrs.release();

        return 1;
    } catch(std::exception& e){
        std::cerr<<"Error: unable to add JDK trust attribute: "<<e.what()<<"\n";
        return 0;
    }
}
#else // !NID_oracle_jdk_trustedkeyusage
static int jdk_trust(PKCS12_SAFEBAG *bag, void *cbarg) noexcept {return 0;}
static inline
PKCS12 *PKCS12_create_ex2(const char *pass, const char *name, EVP_PKEY *pkey,
                          X509 *cert, STACK_OF(X509) *ca, int nid_key, int nid_cert,
                          int iter, int mac_iter, int keytype,
                          OSSL_LIB_CTX *ctx, const char *propq,
                          int (*cb)(PKCS12_SAFEBAG *bag, void *cbarg), void *cbarg)
{
    return PKCS12_create_ex(pass, name, pkey, cert, ca,
                            nid_key, nid_cert, iter, mac_iter, keytype,
                            ctx, propq);
}
#endif // NID_oracle_jdk_trustedkeyusage

/* Understanding X509_EXTENSION in openssl...
 * Each NID_* has a corresponding const X509V3_EXT_METHOD
 * in a crypto/x509/v3_*.c which defines the expected type of the void* value arg.
 *
 * NID_subject_key_identifier   <-> ASN1_OCTET_STRING
 * NID_authority_key_identifier <-> AUTHORITY_KEYID
 * NID_basic_constraints        <-> BASIC_CONSTRAINTS
 * NID_key_usage                <-> ASN1_BIT_STRING
 * NID_ext_key_usage            <-> EXTENDED_KEY_USAGE
 *
 * Use X509V3_CTX automates building these values in the correct way,
 * and than calls low level X509_add1_ext_i2d()
 *
 * see also "man x509v3_config" for explaination of "expr" string.
 */
void add_extension(X509* cert, int nid, const char *expr,
                   const X509* subject = nullptr, const X509* issuer = nullptr)
{
    X509V3_CTX xctx; // well, this is different...
    X509V3_set_ctx_nodb(&xctx);
    X509V3_set_ctx(&xctx, const_cast<X509*>(issuer), const_cast<X509*>(subject), nullptr, nullptr, 0);

    owned_ptr<X509_EXTENSION> ext(X509V3_EXT_conf_nid(nullptr, &xctx, nid,
                                                      expr));
    MUST(1, X509_add_ext(cert, ext.get(), -1));
}

// for writing a PKCS#12 files, right?
struct PKCS12Writer {
    const std::string& outdir;
    const char* friendlyName = nullptr;
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    owned_ptr<STACK_OF(X509)> cacerts;

    explicit PKCS12Writer(const std::string& outdir)
        :outdir(outdir)
        ,cacerts(sk_X509_new_null())
    {}

    void write(const char* fname,
               const char *passwd = "") const {
        owned_ptr<PKCS12> p12(PKCS12_create_ex2(passwd,
                                                friendlyName,
                                                key,
                                                cert,
                                                cacerts.get(),
                                                0, 0, 0, 0, 0,
                                                nullptr, nullptr,
                                                &jdk_trust, nullptr));

        std::string outpath(SB()<<outdir<<fname);
        std::unique_ptr<FILE, ssl_delete<FILE>> out(fopen(outpath.c_str(), "wb"));
        if(!out) {
            auto err = errno;
            throw std::runtime_error(SB()<<"Error opening for write : "<<outpath<<" : "<<strerror(err));
        }

        MUST(1, i2d_PKCS12_fp(out.get(), p12.get()));
    }
};

struct CertCreator {
    // commonName string
    const char *CN = nullptr;
    // NULL for self-signed
    const X509 *issuer = nullptr;
    EVP_PKEY *ikey = nullptr;
    // expiration
    unsigned expire_days = 365*10;
    // cert. serial number
    unsigned serial = 0;
    // extensions
    const char *key_usage = nullptr;
    const char *extended_key_usage = nullptr;
    // Cert. Authority
    bool isCA = false;
    // algorithm attributes
    int keytype = EVP_PKEY_RSA;
    size_t keylen = 2048;
    const EVP_MD* sig = EVP_sha256();

    std::tuple<owned_ptr<EVP_PKEY>, owned_ptr<X509>> create()
    {
        // generate public/private key pair
        owned_ptr<EVP_PKEY> key;
        {
            owned_ptr<EVP_PKEY_CTX> kctx(EVP_PKEY_CTX_new_id(keytype, NULL));
            MUST(1, EVP_PKEY_keygen_init(kctx.get()));
            MUST(1, EVP_PKEY_CTX_set_rsa_keygen_bits(kctx.get(), keylen));
            MUST(1, EVP_PKEY_keygen(kctx.get(), key.acquire()));
        }

        // start assembling certificate
        owned_ptr<X509> cert(X509_new());
        MUST(1, X509_set_version(cert.get(), 2));

        MUST(1, X509_set_pubkey(cert.get(), key.get()));

        // symbolic name for this cert.  Could have multiple entries.
        // but we only add commonName (CN)
        {
            auto sub(X509_get_subject_name(cert.get()));
            if(CN)
                MUST(1, X509_NAME_add_entry_by_txt(sub, "CN", MBSTRING_ASC,
                                                   reinterpret_cast<const unsigned char*>(CN),
                                                   -1, -1, 0));
        }
        if(!issuer) {
            issuer = cert.get(); // self-signed
            ikey = key.get();

        } else if(!ikey) {
            throw std::runtime_error("no issuer key");
        }

        // symbolic name of certificate which issues this new cert.
        MUST(1, X509_set_issuer_name(cert.get(), X509_get_subject_name(issuer)));

        // set valid time range
        {
            time_t now(time(nullptr));
            owned_ptr<ASN1_TIME> before(ASN1_TIME_adj(nullptr, now, 0, -1));
            owned_ptr<ASN1_TIME> after(ASN1_TIME_adj(nullptr, now, expire_days, 0));
            MUST(1, X509_set1_notBefore(cert.get(), before.get()));
            MUST(1, X509_set1_notAfter(cert.get(), after.get()));
        }

        // issuer serial number
        {
            owned_ptr<ASN1_INTEGER> sn(ASN1_INTEGER_new());
            MUST(1, ASN1_INTEGER_set_uint64(sn.get(), serial));
            MUST(1, X509_set_serialNumber(cert.get(), sn.get()));
        }

        // certificate extensions...
        // see RFC5280

        // Store a hash of the public key.  (kind of redundant to stored public key?)
        // RFC5280 mandates this for a CA cert.  Optional for others, and very common.
        add_extension(cert.get(), NID_subject_key_identifier, "hash",
                      cert.get());

        // store hash and name of issuer certificate (or issuer's issuer?)
        // RFC5280 mandates this for all certs.
        add_extension(cert.get(), NID_authority_key_identifier, "keyid:always,issuer:always",
                      nullptr, issuer);

        // certificate usage constraints.

        // most basic.  Can this certificate be an issuer to other certificates?
        // RFC5280 mandates this for a CA cert.  (CA:TRUE)  Optional for others, but common
        add_extension(cert.get(), NID_basic_constraints, isCA ? "critical,CA:TRUE" : "CA:FALSE");

        if(key_usage)
            add_extension(cert.get(), NID_key_usage, key_usage);

        if(extended_key_usage)
            add_extension(cert.get(), NID_ext_key_usage, extended_key_usage);

        auto nbytes(X509_sign(cert.get(), ikey, sig));
        if(nbytes==0)
            throw SSLError("Failed to sign cert");

        return std::make_tuple(std::move(key), std::move(cert));
    }
};

void usage(const char* argv0) {
    std::cerr<<"Usage: "<<argv0<<" [-O <outdir>]\n"
               "\n"
               "    Write out a test of Certificate files for testing.\n"
               "\n"
               "    -O <outdir>  - Write files to this directory.  (default: .)\n"
               ;
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        std::string outdir(".");
        {
            int opt;
            while ((opt = getopt(argc, argv, "hO:")) != -1) {
                switch(opt) {
                case 'h':
                    usage(argv[0]);
                    return 0;
                case 'O':
                    outdir = optarg;
                    if(outdir.empty())
                        throw std::runtime_error("-O argument must not be empty");
                    break;
                default:
                    usage(argv[0]);
                    std::cerr<<"\nUnknown argument: "<<char(opt)<<std::endl;
                    return 1;
                }
            }
        }

        outdir.push_back('/');

        if(optind!=argc) {
            usage(argv[0]);
            std::cerr<<"\nUnexpected arguments\n";
            return 1;
        }

        unsigned serial = 0;

        // The root certificate authority
        owned_ptr<X509> root_cert;
        owned_ptr<EVP_PKEY> root_key;
        {
            CertCreator cc;
            cc.CN = "rootCA";
            cc.serial = serial++;
            cc.isCA = true;
            cc.key_usage = "cRLSign,keyCertSign";

            std::tie(root_key, root_cert) = cc.create();

            PKCS12Writer p12(outdir);
            p12.friendlyName = cc.CN;
            MUST(1, sk_X509_push(p12.cacerts.get(), root_cert.get()));
            p12.write("ca.p12");
            // not saving rootCA key
        }

        // a server-type cert. issued directly from the root
        {
            CertCreator cc;
            cc.CN = "superserver1";
            cc.serial = serial++;
            cc.key_usage = "digitalSignature";
            cc.extended_key_usage = "serverAuth";
            cc.issuer = root_cert.get();
            cc.ikey = root_key.get();

            owned_ptr<X509> cert;
            owned_ptr<EVP_PKEY> key;
            std::tie(key, cert) = cc.create();

            PKCS12Writer p12(outdir);
            p12.friendlyName = cc.CN;
            p12.key = key.get();
            p12.cert = cert.get();
            MUST(1, sk_X509_push(p12.cacerts.get(), root_cert.get()));
            p12.write("superserver1.p12");
        }

        // a chain/intermediate certificate authority
        owned_ptr<X509> i_cert;
        owned_ptr<EVP_PKEY> i_key;
        {
            CertCreator cc;
            cc.CN = "intermediateCA";
            cc.serial = serial++;
            cc.issuer = root_cert.get();
            cc.ikey = root_key.get();
            cc.isCA = true;
            cc.key_usage = "digitalSignature,cRLSign,keyCertSign";
            // on a CA cert. this is a mask of usages which it is allowed to delegate.
            cc.extended_key_usage = "serverAuth,clientAuth,OCSPSigning";

            std::tie(i_key, i_cert) = cc.create();

            PKCS12Writer p12(outdir);
            p12.friendlyName = cc.CN;
            p12.key = i_key.get();
            p12.cert = i_cert.get();
            MUST(1, sk_X509_push(p12.cacerts.get(), root_cert.get()));
            p12.write("intermediateCA.p12");
        }

        // from this point, the rootCA key is no longer needed.
        root_key.reset();

        // remaining certificates issued by intermediate.
        // extendedKeyUsage derived from name: client, server, or IOC (both client and server)
        for(const char *name : {"server1", "server2", "ioc1", "client1", "client2"}) {
            CertCreator cc;
            cc.CN = name;
            cc.serial = serial++;
            cc.key_usage = "digitalSignature";
            if(strstr(name, "server"))
                cc.extended_key_usage = "serverAuth";
            else if(strstr(name, "client"))
                cc.extended_key_usage = "clientAuth";
            else if(strstr(name, "ioc"))
                cc.extended_key_usage = "clientAuth,serverAuth";
            cc.issuer = i_cert.get();
            cc.ikey = i_key.get();

            owned_ptr<X509> cert;
            owned_ptr<EVP_PKEY> key;
            std::tie(key, cert) = cc.create();

            PKCS12Writer p12(outdir);
            p12.friendlyName = cc.CN;
            p12.key = key.get();
            p12.cert = cert.get();
            MUST(1, sk_X509_push(p12.cacerts.get(), i_cert.get()));
            MUST(2, sk_X509_push(p12.cacerts.get(), root_cert.get()));
            std::string fname(SB()<<name<<".p12");

            const char *pw = "";
            if(strcmp(name, "client2")==0)
                pw = "oraclesucks"; // java keytool forces non-interactive IOCs to deal with passwords...

            p12.write(fname.c_str(), pw);
        }

        return 0;
    }catch(std::exception& e){
        std::cerr<<"Error: "<<typeid(e).name()<<" : "<<e.what()<<"\n";
        return 1;
    }
}
