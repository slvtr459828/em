#ifndef EM_HTTP_H
#define EM_HTTP_H
/* HTTP/1.0 and HTTP/1.1 server over an em fd (libem_http.a, link with libem.a). Parsing is
 * picohttpparser (third_party/). Call from a fiber, same rules as em.h.
 *
 * M1 scope: requests without a body, keep-alive, pipelining. A request with Content-Length > 0
 * gets 413, with Transfer-Encoding 501 (both with Content-Length: 400), a header block larger
 * than max_header_bytes 431, a parse error 400; each is answered and the connection closed.
 * No HEAD handling (answer HEAD without a body yourself), no body, no chunked, no timeouts. */
#include <stddef.h>
#include <sys/types.h>

typedef struct em_tls em_tls; /* same type as em_tls.h; M1 requires NULL */

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
} em_http_req;

typedef struct em_http_conn em_http_conn; /* opaque */

/* 0 in a field = default. */
typedef struct em_http_cfg {
    size_t   max_header_bytes;        /* read buffer = request line + headers; default 8192 */
    size_t   max_headers;             /* default 64 */
    unsigned max_requests_per_conn;   /* close after this many; default 0 = unlimited */
} em_http_cfg;

/* Answers the current request with one response; at most once per request. Adds the status
 * line (HTTP/1.minor of the request), Date, Content-Length and, when needed, Connection
 * (keep-alive for a kept 1.0 request, close when the connection ends after this response):
 * pass none of these in h, and no CR/LF in names or values. The response is queued and sent
 * when the next request is not already in the read buffer (pipelined responses go out in one
 * send) or the 8 KiB write buffer is full; a body that does not fit it goes straight from
 * `body` in a second send, so `body` must stay valid until respond returns.
 * 0, -EALREADY, -EINVAL (status not 100..999), -EMSGSIZE (headers over the write buffer),
 * or the send error (then the connection is dead and serve returns it). */
int em_http_respond(em_http_conn *c, int status, const em_http_header *h, size_t nh,
                    const void *body, size_t len);

/* The handler runs on the connection's fiber. It returns 0 after calling em_http_respond. A
 * nonzero return, or 0 without a response, is answered 500 and the connection closed (a
 * nonzero return is also what em_http_serve returns). */
typedef int (*em_http_handler)(em_http_conn *c, const em_http_req *r, void *arg);

/* Serves fd until the peer closes (0), the connection must close (0), or an error (-errno:
 * -ECANCELED/-ETIMEDOUT from the scope, a send/recv error, -ECONNRESET if the peer closed in
 * the middle of a request, the handler's value). Does not close fd. One read buffer per call,
 * nothing allocated per request. tls must be NULL (-ENOTSUP otherwise). */
int em_http_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg);

#endif
