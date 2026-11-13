#ifndef EM_HTTP_H
#define EM_HTTP_H
/* HTTP/1.0 and HTTP/1.1 server over an em fd (libem_http.a, link with libem.a). Parsing is
 * picohttpparser (third_party/). Call from a fiber, same rules as em.h.
 *
 * Scope: keep-alive, pipelining, request bodies (Content-Length or chunked, read with
 * em_http_read_body), Expect: 100-continue, HEAD, limits and timeouts. Refused requests are
 * answered and the connection closed: a parse error 400; both Transfer-Encoding and
 * Content-Length, a repeated Content-Length with another value or an invalid one 400; a
 * Transfer-Encoding other than "chunked" 501; Expect other than 100-continue 417; a
 * Content-Length over max_body_bytes 413; a folded header line (obs-fold) 400; a header block
 * over max_header_bytes 431.
 * Plain or TLS (see em_http_serve). Not here: streamed responses (one em_http_respond per request). */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct em_tls em_tls; /* same type as em_tls.h */

/* Layout is that of picohttpparser's phr_header (checked in http.c). */
typedef struct em_http_header {
    const char *name;  size_t name_len;
    const char *value; size_t value_len;
} em_http_header;

/* Points into the connection's read buffer: valid only while the handler runs. */
typedef struct em_http_req {
    const char *method, *path;  size_t method_len, path_len;
    int minor_version;                    /* 0 = HTTP/1.0, 1 = HTTP/1.1 */
    const em_http_header *headers;
    size_t nheaders;
    int64_t content_length;               /* body: Content-Length; 0 = no body; -1 = chunked */
} em_http_req;

typedef struct em_http_conn em_http_conn; /* opaque */

/* 0 in a field = default. */
typedef struct em_http_cfg {
    size_t   max_header_bytes;        /* read buffer = request line + headers; default 8192 */
    size_t   max_headers;             /* default 64 */
    unsigned max_requests_per_conn;   /* close after this many; default 0 = unlimited */
    uint64_t max_body_bytes;          /* default 1 MiB: a larger Content-Length is answered 413
                                       * before the handler; a chunked body over it makes
                                       * em_http_read_body return -EMSGSIZE */
    int64_t  header_timeout_ns;       /* first byte of a request to the end of its head; default
                                       * 30 s; -1 = none. Expiry: 408, then close */
    int64_t  idle_timeout_ns;         /* wait for the next request on a kept connection, and each
                                       * wait for body bytes; default 60 s; -1 = none. Expiry
                                       * between requests: close without a response */
    int64_t  send_timeout_ns;         /* a send that moves no byte for this long drops the
                                       * connection (like nginx send_timeout: a big response to a
                                       * slow reader that keeps taking data is not cut); default
                                       * 60 s; -1 = none. Applies to every send serve makes: the
                                       * response, 100 Continue, error answers. Under TLS each
                                       * send is at most 64 KiB (em_tls_send_all has no partial
                                       * result) and has its own deadline, and an expiry breaks the
                                       * em_tls (its calls then return -EPIPE: close it). No partial
                                       * response is followed by anything: serve returns -ETIMEDOUT,
                                       * em_http_respond returns it to the handler first */
} em_http_cfg;

/* Answers the current request with one response; at most once per request. Adds the status
 * line (HTTP/1.minor of the request), Date, Content-Length and, when needed, Connection
 * (keep-alive for a kept 1.0 request, close when the connection ends after this response):
 * pass none of these in h, and no CR/LF in names or values. The response is queued and sent
 * when the next request is not already in the read buffer (pipelined responses go out in one
 * send) or the 8 KiB write buffer is full; a body that does not fit it goes straight from
 * `body` in a second send, so `body` must stay valid until respond returns.
 * HEAD: the same headers (Content-Length = len), no body sent.
 * A body the handler left unread: up to 64 KiB of Content-Length is drained after the response
 * and the connection kept; more than that, an unfinished chunked body, or an Expect:
 * 100-continue body never asked for makes this response carry Connection: close.
 * 0, -EALREADY, -EINVAL (status not 100..999), -EMSGSIZE (headers over the write buffer),
 * or the send error (then the connection is dead and serve returns it). */
int em_http_respond(em_http_conn *c, int status, const em_http_header *h, size_t nh,
                    const void *body, size_t len);

/* Reads the request body, decoded (chunked framing removed): > 0 bytes, 0 = end of body (also
 * for a request without one). The first call sends "100 Continue" when the request asked for it.
 * Call before em_http_respond (-EALREADY after it). Errors: -EMSGSIZE (chunked body over
 * max_body_bytes), -EPROTO (bad chunked framing), -ECONNRESET (peer closed mid-body),
 * -ETIMEDOUT (idle_timeout or the scope deadline), -ECANCELED, a recv error. After an error the
 * connection closes when the handler returns; without a response from the handler, serve answers
 * 413 for -EMSGSIZE, 400 for -EPROTO, 408 for -ETIMEDOUT, nothing for the others. */
ssize_t em_http_read_body(em_http_conn *c, void *buf, size_t len);

/* The handler runs on the connection's fiber. It returns 0 after calling em_http_respond. A
 * nonzero return, or 0 without a response, is answered 500 and the connection closed (a
 * nonzero return is also what em_http_serve returns). */
typedef int (*em_http_handler)(em_http_conn *c, const em_http_req *r, void *arg);

/* Serves fd until the peer closes (0), the connection must close (0), or an error (-errno:
 * -ECANCELED/-ETIMEDOUT from the scope or send_timeout, a send/recv error, -ECONNRESET if the peer
 * closed in the middle of a request, the handler's value). Does not close fd. One read buffer per
 * call, nothing allocated per request.
 *
 * tls: NULL = plain TCP. Otherwise the caller already ran em_tls_accept on fd; serve reads and
 * writes through em_tls_recv / em_tls_send_all and never sends close_notify, shuts down, frees or
 * closes it: that stays with the caller. Passing a non-NULL tls requires linking libem_tls (libem_http
 * itself does not reference OpenSSL: a plain-only program links without it). Over TLS a TCP close
 * without close_notify between two requests ends serve with 0, not -ECONNRESET.
 *
 * Lingering close: when serve ends the connection while the peer may still be sending request
 * bytes it will not read (413 before the handler; 400/431/501/417 on a request still arriving; a
 * 500 or a Connection: close answer over an unread body; requests already pipelined behind the last
 * one max_requests_per_conn allows), close() by the caller would make the kernel
 * send RST and the client could lose the response. So after the final response serve calls
 * shutdown(fd, SHUT_WR) on the raw fd and discards what the peer sends until it closes, 2 s or
 * 1 MiB (whichever first; the caller's scope cancel or deadline also ends it). Under TLS those are
 * raw bytes and the write side is shut, so a close_notify sent afterwards fails: ignore it. Not
 * done for normal closes (request fully read), idle and header timeouts, or peer EOF/errors. */
int em_http_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg);

#endif
