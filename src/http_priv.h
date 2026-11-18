#ifndef EM_HTTP_PRIV_H
#define EM_HTTP_PRIV_H
/* The seam between http.c (libem_http.a) and http2.c (libem_http2.a). Private. */
#include <stdint.h>
#include "em_http.h"

/* em_tls.h pulls in OpenSSL, which a plain-HTTP user need not have, so the calls made here are
 * declared again, weak: tls != NULL means the caller got it from em_tls_accept and so linked
 * libem_tls, which binds them; with tls == NULL they are never reached. Keep in step with em_tls.h. */
ssize_t em_tls_recv(em_tls *t, void *buf, size_t len) __attribute__((weak));
int     em_tls_send_all(em_tls *t, const void *buf, size_t len) __attribute__((weak));
const unsigned char *em_tls_alpn(em_tls *t, unsigned *len) __attribute__((weak));

/* Defined in http.c, used by http2.c. */
const char *em_http_date_line(void); /* "Date: Thu, 12 Nov 2026 12:00:00 GMT\r\n" (DATE_LEN = 37 bytes) */
char *em_http_put_u64(char *p, uint64_t v);
/* Every send either protocol makes: TLS or plain, a deadline per call that makes progress (0 = none). */
int em_http_send_all(int fd, em_tls *tls, int64_t send_ns, const char *p, size_t len);

/* Lingering close (see em_http_serve in em_http.h): FIN, then discard what the peer still sends. */
void em_http_linger(int fd, void *buf, size_t cap);

/* Defined in http2.c, bound weak from http.c.
 * serve: the connection speaks HTTP/2; pre/prelen = bytes already read from it (h2c preface seen). */
int     em_http2_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg,
                       const void *pre, size_t prelen);
/* em_http_respond / em_http_read_body on an em_http_conn whose first int is 1. */
int     em_http2_respond(em_http_conn *c, int status, const em_http_header *h, size_t nh,
                         const void *body, size_t len);
ssize_t em_http2_read_body(em_http_conn *c, void *buf, size_t len);
#endif
