#ifndef EM_TLS_H
#define EM_TLS_H
/* TLS over an em fd: OpenSSL driven through a BIO pair, every wait goes through
 * em_recv / em_send. Link libem_tls.a, libem.a, -lssl -lcrypto. libem itself does not
 * depend on OpenSSL. Same fiber context rules as em.h: call from a fiber.
 *
 * Library. Default: OpenSSL >= 3.0 found by pkg-config. `make TLS_PREFIX=dir` builds against the
 * library installed in dir (include/, lib/ or lib64/) instead: a self-built OpenSSL, or AWS-LC
 * (tested: v5.12.0; bench/tls_libs.sh builds it: BUILD_ONLY=1 LIBS=aws-lc-5.12.0). The SSL_CTX you
 * pass in must come from the same library em_tls was built against. AWS-LC differences you can
 * see: no stateful TLS 1.3 tickets (SSL_OP_NO_TICKET gives no resumption); a TLS 1.3 server sends
 * its tickets with its next write, so a server that only reads never issues one; renegotiation is
 * off (as em_tls wants anyway). Symbols: AWS-LC reuses OpenSSL's names, so a process that links
 * em_tls with AWS-LC AND the system OpenSSL through another library (libcurl, libpq...) will have
 * one side call into the other. Build AWS-LC with BORINGSSL_PREFIX to avoid it (documented here,
 * not built or tested by the repo's CI).
 *
 * Ownership.
 *  - fd: a connected TCP socket from em (em_accept / em_connect). em_tls never closes it.
 *    Close the fd (em_close) after em_tls_free, on the worker that owns it; use of an
 *    em_tls from another worker is not supported. Nothing else may read or write fd while
 *    the em_tls lives (it would steal or corrupt records).
 *  - ctx: the caller's SSL_CTX (certs, ALPN, verify mode, ciphers: plain OpenSSL API).
 *    em_tls takes its own reference through SSL_new; the ctx may be shared by every
 *    worker and may be freed once all em_tls made from it are freed.
 *  - em_tls_free does not send close_notify (call em_tls_shutdown first) and must not run
 *    while another fiber is inside a call on the same em_tls.
 *  - em_tls sets SSL_OP_NO_RENEGOTIATION on its SSL (renegotiation would let a write wait
 *    for a read). TLS 1.3 KeyUpdate is handled normally.
 *
 * Duplex. One em_tls allows at most ONE fiber in em_tls_recv and ONE fiber in
 * em_tls_send_all / em_tls_shutdown at the same time (proxy, WebSocket, HTTP/2 style:
 * one reads while another writes). A second concurrent reader or writer gets -EBUSY.
 * Handshake calls are exclusive: nobody else has the em_tls yet.
 * Do not call SSL_* on em_tls_ssl() while a call is in progress on another fiber, except
 * what OpenSSL permits between reads and writes (SSL_key_update, getters).
 *
 * Cancellation (scope cancel or deadline: -ECANCELED / -ETIMEDOUT).
 *  - while em_tls_recv waits for bytes: nothing was consumed, the em_tls is intact, call
 *    again (this is how a read deadline is used).
 *  - while sending: a TLS record may be half on the wire and OpenSSL's write state is
 *    mid-record, so the stream is broken. This includes the flush of a record that
 *    em_tls_recv itself had to send (KeyUpdate answer, alert). From then on every call
 *    returns -EPIPE; close the connection. The call that failed returns the cause.
 *  - handshake: -ECANCELED / -ETIMEDOUT, the em_tls is not created.
 *
 * Known limit: both ends flushing a record from inside em_tls_recv at the same moment
 * with both TCP buffers full (simultaneous KeyUpdate under bulk traffic) can stall, as
 * neither is reading. */
#include <openssl/ssl.h>
#include <sys/types.h>

typedef struct em_tls em_tls;

/* Handshake over fd. For connect, host sets SNI and, if the SSL verifies peers
 * (SSL_CTX_set_verify), the expected name (SSL_set1_host; an IP literal is checked as an
 * IP address and sends no SNI); NULL skips both.
 * Returns 0 and *out, or -errno: -EPROTO handshake failed (alert, bad certificate; the
 * OpenSSL error queue of this thread has the detail), -ECONNRESET peer closed,
 * -ECANCELED / -ETIMEDOUT, -ENOMEM, or the error of em_recv / em_send.
 * On error nothing is allocated and *out is untouched. */
int     em_tls_accept(em_tls **out, SSL_CTX *ctx, int fd);
int     em_tls_connect(em_tls **out, SSL_CTX *ctx, int fd, const char *host);

/* >0 bytes read; 0 = peer sent close_notify (clean end); -ECONNRESET = TCP closed
 * without close_notify (truncation, never reported as 0); -EPROTO; -EBUSY; -EPIPE (see
 * cancellation); -ECANCELED / -ETIMEDOUT; len must be > 0 (-EINVAL). */
ssize_t em_tls_recv(em_tls *t, void *buf, size_t len);
/* 0 when every byte is encrypted AND handed to the kernel (em_send), else -errno as above. */
int     em_tls_send_all(em_tls *t, const void *buf, size_t len);
/* Sends close_notify (does not wait for the peer's). */
int     em_tls_shutdown(em_tls *t);
void    em_tls_free(em_tls *t);
/* ALPN, peer certificate, session, SSL_key_update. Owned by t. */
SSL    *em_tls_ssl(em_tls *t);

#endif
