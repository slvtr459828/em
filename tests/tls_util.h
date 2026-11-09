#ifndef EM_TEST_TLS_UTIL_H
#define EM_TEST_TLS_UTIL_H
/* Self-signed EC P-256 certificate for "localhost", generated in memory with the OpenSSL API
 * (no files, no openssl CLI). Server and client contexts share it; the client trusts only it. */
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include "util.h"
#include "../include/em_tls.h"

static inline void tls_test_ctx(SSL_CTX **srv, SSL_CTX **cli)
{
    EVP_PKEY *key = EVP_EC_gen("P-256");
    assert(key);
    X509 *x = X509_new();
    assert(x);
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
    X509_set_pubkey(x, key);
    X509_NAME *nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, "DNS:localhost");
    assert(san && X509_add_ext(x, san, -1));
    X509_EXTENSION_free(san);
    assert(X509_sign(x, key, EVP_sha256()) > 0);

    *srv = SSL_CTX_new(TLS_server_method());
    assert(*srv && SSL_CTX_use_certificate(*srv, x) == 1 && SSL_CTX_use_PrivateKey(*srv, key) == 1);
    *cli = SSL_CTX_new(TLS_client_method());
    assert(*cli);
    X509_STORE *st = SSL_CTX_get_cert_store(*cli);
    assert(X509_STORE_add_cert(st, x) == 1);
    SSL_CTX_set_verify(*cli, SSL_VERIFY_PEER, NULL);
    X509_free(x);
    EVP_PKEY_free(key);
}
#endif
