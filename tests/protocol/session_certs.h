#ifndef LOGOS_TEST_SESSION_CERTS_H
#define LOGOS_TEST_SESSION_CERTS_H

// In-memory P-256 roots and leaves for tls_tcp tests: a leaf carries the
// serverAuth or clientAuth EKU its side needs, and nothing identifies anyone.

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace session_test {

struct KeyDeleter { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
struct CertDeleter { void operator()(X509* c) const { X509_free(c); } };
using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
using Cert = std::unique_ptr<X509, CertDeleter>;

inline Key newKey()
{
    Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!key) throw std::runtime_error("keygen");
    return key;
}

inline void addExt(X509* cert, X509* issuer, int nid, const char* value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) throw std::runtime_error("extension");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

inline Cert makeCert(EVP_PKEY* subject, EVP_PKEY* signer, X509* issuer, bool ca, const char* eku)
{
    Cert cert(X509_new());
    X509_set_version(cert.get(), 2);
    static long serial = 1000;
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(ca ? "test-root" : "test-leaf"),
                               -1, -1, 0);
    X509* issuerCert = issuer ? issuer : cert.get();
    X509_set_issuer_name(cert.get(), X509_get_subject_name(issuerCert));
    X509_set_pubkey(cert.get(), subject);
    if (ca) {
        addExt(cert.get(), issuerCert, NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
        addExt(cert.get(), issuerCert, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        addExt(cert.get(), issuerCert, NID_basic_constraints, "critical,CA:FALSE");
        addExt(cert.get(), issuerCert, NID_key_usage, "critical,digitalSignature");
        addExt(cert.get(), issuerCert, NID_ext_key_usage, eku);
    }
    addExt(cert.get(), issuerCert, NID_subject_key_identifier, "hash");
    addExt(cert.get(), issuerCert, NID_authority_key_identifier, "keyid:always");
    if (X509_sign(cert.get(), signer, EVP_sha256()) <= 0) throw std::runtime_error("sign");
    return cert;
}

inline std::string pem(X509* cert)
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return out;
}

inline std::string pem(EVP_PKEY* key)
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return out;
}

inline std::string b64url(const unsigned char* data, std::size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        acc = ((acc << 8) | data[i]) & 0xFFFFFFu;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(acc >> bits) & 63u]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(acc << (6 - bits)) & 63u]);
    return out;
}

inline std::string pin(X509* cert)
{
    unsigned char* der = nullptr;
    const int size = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(der, static_cast<std::size_t>(size), hash, &length, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    return "sha256:" + b64url(hash, length);
}

inline std::string der64(X509* cert)
{
    unsigned char* der = nullptr;
    const int size = i2d_X509(cert, &der);
    std::string out = b64url(der, static_cast<std::size_t>(size));
    OPENSSL_free(der);
    return out;
}

// A root and one leaf of the given usage.
struct Identity {
    Key rootKey = newKey();
    Cert root = makeCert(rootKey.get(), rootKey.get(), nullptr, true, nullptr);
    Key leafKey = newKey();
    Cert leaf;
    explicit Identity(const char* eku)
        : leaf(makeCert(leafKey.get(), rootKey.get(), root.get(), false, eku)) {}
    std::string chainPem() const { return pem(leaf.get()) + pem(root.get()); }
    std::string keyPem() const { return pem(leafKey.get()); }
    std::string rootPem() const { return pem(root.get()); }
    std::string leafPin() const { return pin(leaf.get()); }
};

} // namespace session_test

#endif
