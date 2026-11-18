#ifndef EM_TEST_H2_UTIL_H
#define EM_TEST_H2_UTIL_H
/* HTTP/2 client for the tests: nghttp2's client session over an em fd (plain = h2c prior knowledge,
 * -DHTTP_TLS = h2 through ALPN), driven by one fiber: h2_run sends what nghttp2 has, receives, and
 * returns when every request made so far is closed. Responses are kept per request. */
#include <nghttp2/nghttp2.h>
#include "http_util.h"

#define H2_MAX  80
#define H2_BODY 32768 /* "/big" is 20000 bytes */

typedef struct {
    int      status, closed, order; /* order: 1 = the first stream to close */
    uint32_t err;                   /* RST_STREAM / close error code */
    int      has_date, has_conn;
    int64_t  clen;                  /* content-length, -1 = none */
    size_t   blen;
    char     body[H2_BODY];
    const char *req;                /* request body, if any */
    size_t   req_len, req_off;
} h2resp;

typedef struct { rd_t *r; nghttp2_session *ses; h2resp *resp; int n, open, seq, settings; } h2cl;

static inline ssize_t h2_req_body(nghttp2_session *s, int32_t id, uint8_t *buf, size_t len, uint32_t *fl,
                           nghttp2_data_source *src, void *ud)
{
    (void)s; (void)id; (void)ud;
    h2resp *p = src->ptr;
    size_t n = p->req_len - p->req_off < len ? p->req_len - p->req_off : len;
    memcpy(buf, p->req + p->req_off, n);
    p->req_off += n;
    if (p->req_off == p->req_len) *fl |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static inline int h2_on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *nm, size_t nl,
                        const uint8_t *v, size_t vl, uint8_t fl, void *ud)
{
    (void)ud; (void)fl;
    h2resp *p = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
    if (nl == 7 && !memcmp(nm, ":status", 7)) p->status = atoi((const char *)v);
    else if (nl == 4 && !memcmp(nm, "date", 4)) p->has_date = 1;
    else if (nl == 10 && !memcmp(nm, "connection", 10)) p->has_conn = 1;
    else if (nl == 14 && !memcmp(nm, "content-length", 14)) p->clen = atoll((const char *)v);
    (void)vl;
    return 0;
}

static inline int h2_on_data(nghttp2_session *s, uint8_t fl, int32_t id, const uint8_t *d, size_t n, void *ud)
{
    (void)fl; (void)ud;
    h2resp *p = nghttp2_session_get_stream_user_data(s, id);
    assert(p->blen + n <= H2_BODY);
    memcpy(p->body + p->blen, d, n);
    p->blen += n;
    return 0;
}

static inline int h2_on_close(nghttp2_session *s, int32_t id, uint32_t code, void *ud)
{
    h2cl *c = ud;
    h2resp *p = nghttp2_session_get_stream_user_data(s, id);
    p->closed = 1;
    p->err = code;
    p->order = ++c->seq;
    c->open--;
    return 0;
}

static inline int h2_on_frame(nghttp2_session *s, const nghttp2_frame *f, void *ud)
{
    (void)s;
    if (f->hd.type == NGHTTP2_SETTINGS && !(f->hd.flags & NGHTTP2_FLAG_ACK)) ((h2cl *)ud)->settings = 1;
    return 0;
}

/* Raw connection to the server: TLS with ALPN "h2" under -DHTTP_TLS (checked), else plain. */
static inline rd_t *h2_connect(uint16_t port)
{
    rd_t *r = calloc(1, sizeof *r);
    r->fd = connect_local(port);
    assert(r->fd >= 0);
#ifdef HTTP_TLS
    assert(em_tls_connect(&r->t, http_h2ctx, r->fd, "localhost") == 0);
    const unsigned char *p;
    unsigned n;
    SSL_get0_alpn_selected(em_tls_ssl(r->t), &p, &n);
    assert(n == 2 && !memcmp(p, "h2", 2));
#endif
    return r;
}

static inline void h2_pump(h2cl *c, int until_closed);

static inline h2cl *h2_open(uint16_t port)
{
    h2cl *c = calloc(1, sizeof *c);
    c->r = h2_connect(port);
    c->resp = calloc(H2_MAX, sizeof *c->resp);
    nghttp2_session_callbacks *cb;
    assert(nghttp2_session_callbacks_new(&cb) == 0);
    nghttp2_session_callbacks_set_on_header_callback(cb, h2_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, h2_on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, h2_on_close);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, h2_on_frame);
    assert(nghttp2_session_client_new(&c->ses, cb, c) == 0);
    nghttp2_session_callbacks_del(cb);
    assert(nghttp2_submit_settings(c->ses, NGHTTP2_FLAG_NONE, NULL, 0) == 0);
    h2_pump(c, 0);
    return c;
}

#define H2_NV(n, v, vl) { (uint8_t *)(n), (uint8_t *)(v), sizeof n - 1, vl, NGHTTP2_NV_FLAG_NONE }

/* Queues a request; with body != NULL it is sent as DATA. */
static inline h2resp *h2_req(h2cl *c, const char *method, const char *path, const char *body, size_t blen)
{
    assert(c->n < H2_MAX);
    h2resp *p = &c->resp[c->n++];
    p->clen = -1;
    nghttp2_nv nv[] = { H2_NV(":method", method, strlen(method)), H2_NV(":scheme", "http", 4),
                        H2_NV(":authority", "localhost", 9), H2_NV(":path", path, strlen(path)) };
    nghttp2_data_provider prd = { .source.ptr = p, .read_callback = h2_req_body };
    p->req = body; p->req_len = blen;
    assert(nghttp2_submit_request(c->ses, NULL, nv, 4, body ? &prd : NULL, p) > 0);
    c->open++;
    return p;
}

/* Sends what nghttp2 has and receives until every request is closed (until_closed) or, if not, until
 * the server's SETTINGS are in (a client that waits for them knows the stream limit). */
static inline void h2_pump(h2cl *c, int until_closed)
{
    for (;;) {
        for (;;) {
            const uint8_t *d;
            ssize_t n = nghttp2_session_mem_send(c->ses, &d);
            assert(n >= 0);
            if (!n) break;
            assert(rd_send(c->r, d, (size_t)n) == 0);
        }
        if (until_closed ? !c->open : c->settings) return;
        char buf[16384];
        ssize_t n = rd_recv(c->r, buf, sizeof buf);
        assert(n > 0);
        assert(nghttp2_session_mem_recv(c->ses, (uint8_t *)buf, (size_t)n) == n);
    }
}

static inline void h2_run(h2cl *c) { h2_pump(c, 1); }

static inline void h2_close(h2cl *c)
{
    nghttp2_session_del(c->ses);
    rd_close(c->r);
    free(c->resp);
    free(c);
}

/* status, exact body, content-length = body length, a Date and no Connection header */
static inline void h2_expect(const h2resp *p, int status, const char *body)
{
    if (!p->closed || p->err || p->status != status || p->blen != strlen(body) ||
        memcmp(p->body, body, p->blen) || p->clen != (int64_t)strlen(body) || !p->has_date || p->has_conn) {
        fprintf(stderr, "h2: want %d \"%.40s\" got status %d closed %d err %u body %zu bytes clen %lld date %d conn %d\n",
                status, body, p->status, p->closed, p->err, p->blen, (long long)p->clen, p->has_date, p->has_conn);
        abort();
    }
}
#endif
