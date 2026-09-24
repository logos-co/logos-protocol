#ifndef LOGOS_TEST_SELF_SIGNED_CERT_H
#define LOGOS_TEST_SELF_SIGNED_CERT_H

// A throwaway certificate for 127.0.0.1, for the tests' TLS listeners.

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>

inline bool writeSelfSignedCert(const std::filesystem::path& certPath,
                                const std::filesystem::path& keyPath, bool rsa = false)
{
    EVP_PKEY* key = rsa ? EVP_RSA_gen(2048) : EVP_EC_gen("P-256");
    X509* cert = X509_new();
    bool ok = key && cert;
    if (ok) {
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        ok = X509_sign(cert, key, EVP_sha256()) > 0;
    }
    if (ok) {
        FILE* keyFile = std::fopen(keyPath.string().c_str(), "wb");
        FILE* certFile = std::fopen(certPath.string().c_str(), "wb");
        ok = keyFile && certFile
            && PEM_write_PrivateKey(keyFile, key, nullptr, nullptr, 0, nullptr, nullptr)
            && PEM_write_X509(certFile, cert);
        if (keyFile) std::fclose(keyFile);
        if (certFile) std::fclose(certFile);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

#endif // LOGOS_TEST_SELF_SIGNED_CERT_H
