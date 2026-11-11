/* The bench certificate (ECDSA P-256, self-signed, generated at startup) from tests/tls_util.h, as PEM
 * for Photon's TLSContext. C file because tls_util.h is C. */
#include <string.h>
#include "../../../tests/tls_util.h"

static char *pem(BIO *b)
{
    char *p;
    long n = BIO_get_mem_data(b, &p);
    char *s = malloc((size_t)n + 1);
    assert(s);
    memcpy(s, p, (size_t)n);
    s[n] = 0;
    BIO_free(b);
    return s;
}

void photon_cert_pem(char **cert, char **key)
{
    SSL_CTX *srv, *cli;
    tls_test_ctx(&srv, &cli);
    BIO *bc = BIO_new(BIO_s_mem()), *bk = BIO_new(BIO_s_mem());
    assert(bc && bk);
    assert(PEM_write_bio_X509(bc, SSL_CTX_get0_certificate(srv)) == 1);
    assert(PEM_write_bio_PrivateKey(bk, SSL_CTX_get0_privatekey(srv), NULL, NULL, 0, NULL, NULL) == 1);
    *cert = pem(bc);
    *key = pem(bk);
    SSL_CTX_free(srv);
    SSL_CTX_free(cli);
}
