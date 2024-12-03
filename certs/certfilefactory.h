#ifndef PVXS_CERT_FILE_FACTORY_H
#define PVXS_CERT_FILE_FACTORY_H

#include <memory>
#include <string>
#include <utility>

#include <openssl/x509.h>

#include <pvxs/log.h>

#include "ownedptr.h"
#include "security.h"

namespace pvxs {
namespace certs {

// Forward declarations
class P12FileFactory;
class PEMFileFactory;

// C++11 implementation of make_unique
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// CertData structure definition
struct CertData {
    ossl_ptr<X509> cert;
    ossl_shared_ptr<STACK_OF(X509)> ca;
    std::shared_ptr<KeyPair> key_pair;

    CertData(ossl_ptr<X509>& newCert, ossl_shared_ptr<STACK_OF(X509)>& newCa) : cert(std::move(newCert)), ca(newCa) {}
    CertData(ossl_ptr<X509>& newCert, ossl_shared_ptr<STACK_OF(X509)>& newCa, std::shared_ptr<KeyPair> key_pair)
        : cert(std::move(newCert)), ca(newCa), key_pair(key_pair) {}
};

/**
 * @brief The availability of a certificate file
 *
 * This is returned when when authentication daemons are trying to provision the configured certificate files.
 *  - `NOT_AVAILABLE` is returned if the file does not exist and can't be provisioned.
 *  - `ROOT_CERT_INSTALLED` is returned if the file exists or has been provisioned but
 *     the root CA certificate was downloaded and installed during the call.  This signals to the caller
 *     the configured certificate will be unusable until the user trusts the root CA certificate.
 *  - `AVAILABLE` is returned if the file already exists.
 *  - `OK` is returned if the certificate file was provisioned and is ready for use.
 */
enum CertAvailability {
    OK,
    NOT_AVAILABLE,
    ROOT_CERT_INSTALLED,
    AVAILABLE,
};

class CertFileFactory {
   public:
    /**
     * @brief Creates a new CertFileFactory object.
     *
     * This method creates a new CertFileFactory object.
     */
    static std::unique_ptr<CertFileFactory> create(const std::string& filename, const std::string& password = "",
                                                   const std::shared_ptr<KeyPair>& key_pair = nullptr, X509* cert_ptr = nullptr,
                                                   STACK_OF(X509) * certs_ptr = nullptr, const std::string& usage = "certificate",
                                                   const std::string& pem_string = "",
                                                   bool certs_only = false);  // Move implementation to cpp file

    virtual ~CertFileFactory() = default;

    /**
     * @brief Writes the certificate file.
     *
     * This method writes the certificate file.
     * The format is determined by the filename extension.
     */
    virtual void writeCertFile() = 0;

    /**
     * @brief Gets the certificate data from the file.
     *
     * This method gets the certificate data from the file.
     * The format is determined by the filename extension.
     */
    virtual CertData getCertDataFromFile() = 0;

    /**
     * @brief Gets the key from the file.
     *
     * This method gets the key from the file.
     * The format is determined by the filename extension.
     */
    virtual std::shared_ptr<KeyPair> getKeyFromFile() = 0;

    /**
     * @brief Creates a key pair.
     *
     * This method creates a key pair.  Private key is generated and public key is extracted from the private key.
     */
    static std::shared_ptr<KeyPair> createKeyPair();

    /**
     * @brief Writes a root PEM file.
     *
     * This method writes a root PEM file
     */
    bool writeRootPemFile(const std::string& pem_string, bool overwrite = false);
    CertData getCertData(const std::shared_ptr<KeyPair>& key_pair);

   protected:
    CertFileFactory(const std::string& filename, X509* cert_ptr = nullptr, STACK_OF(X509) * certs_ptr = nullptr, const std::string& usage = "certificate",
                    const std::string& pem_string = "", bool certs_only = false)
        : filename_(filename), cert_ptr_(cert_ptr), certs_ptr_(certs_ptr), usage_(usage), pem_string_(pem_string), certs_only_(certs_only) {}

    const std::string filename_;
    X509* cert_ptr_{nullptr};
    STACK_OF(X509) * certs_ptr_ { nullptr };
    const std::string usage_;
    const std::string pem_string_;
    const bool certs_only_{false};

    static void backupFileIfExists(const std::string& filename);
    static void chainFromRootCertPtr(STACK_OF(X509) * &chain, X509* root_cert_ptr);
    static std::string getExtension(const std::string& filename) { return filename.substr(filename.find_last_of(".") + 1); };
};

}  // namespace certs
}  // namespace pvxs

#endif
