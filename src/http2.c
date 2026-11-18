/* HTTP/2 server (libem_http2.a): nghttp2 does framing, HPACK, flow control and stream state; em does
 * the I/O. See plans/http2.md. em_http_serve calls em_http2_serve (weak, http.c) when the connection
 * speaks h2: TLS with ALPN "h2", or the h2c client preface on a plain one.
 *
 * Three kinds of fiber per connection, all in one scope and all on one worker (nghttp2 is never
 * entered from two fibers at once: none of them parks inside a call):
 *   reader  recv, nghttp2_session_mem_recv. The callbacks only fill in stream state and spawn.
 *   writer  parks until there is output, drains nghttp2_session_mem_send into one buffer, sends.
 *   stream  one per request, runs the user's handler. em_http_respond hands the response to nghttp2,
 *           wakes the writer and parks until the stream closes (done, or reset).
 * The reader is a child fiber (not the caller's) so that the writer can end it: a scope owner is
 * not interrupted by its own scope's cancel. M1: a request without body (GET); one with a body is
 * answered 501. nghttp2 >= 1.57 only has the non-"2" mem_recv/mem_send, which newer versions keep. */
#include <errno.h>
#include <nghttp2/nghttp2.h>
#include <stdlib.h>
#include <string.h>
#include "em.h"
#include "http_priv.h"

#if NGHTTP2_VERSION_NUM < 0x013900
#error "libem_http2 needs nghttp2 >= 1.57 (Rapid Reset, CVE-2023-44487)"
#endif

#define RBUF        16384u
#define WBUF        32768u
#define DEF_HCAP    8192u
#define DEF_HDRS    64u
#define DEF_STREAMS 100u
#define DEF_SEND    60000000000ll
#define MAX_RESP_HDRS 64  /* user headers in one response */
#define LC_BYTES    1024  /* their lower-cased names (h2 names are lower case) */
#define DATE_VAL    29u   /* "Thu, 12 Nov 2026 12:00:00 GMT" */
/* CONTINUATION frames in one header block (CVE-2024-27983/28182: nghttp2 < 1.62 does not count them,
 * and a distro may or may not have backported the fix). A block of max_header_bytes needs one. */
#define MAX_CONT    16u

struct h2c;
typedef struct h2s {
    int     h2;                 /* 1: what http.c tests in an em_http_conn */
    uint8_t closed;             /* nghttp2 closed the stream: it no longer points here */
    uint8_t done;               /* no fiber points here (none yet, or it returned) */
    uint8_t responded, dead, head;
    uint32_t err;               /* RST/close error code */
    int32_t id;
    struct h2c *c;
    struct h2s *prev, *next;
    em_fiber *waiter;           /* the handler, parked in respond */
    const char *body;           /* the response body: nghttp2 copies from it while it sends */
    size_t  blen, boff;
    size_t  nh, hn;             /* request headers so far, bytes used in hb */
    em_http_req req;
    em_http_header *hv;         /* max_headers entries, then hb: one malloc with the struct */
    char   *hb;                 /* max_header_bytes: method, path, header names and values */
} h2s;

struct h2c {
    int     fd;
    em_tls *tls;
    int64_t send_ns;            /* 0 = none */
    nghttp2_session *ses;
    em_http_handler h;
    void   *arg;
    em_scope *sc;
    em_fiber *wf;               /* writer, while parked */
    h2s    *streams;            /* every stream still pointed to by nghttp2 or by a fiber */
    unsigned nstreams, maxstreams, cont;
    size_t  maxh, hcap;
    const void *pre;            /* bytes the h1 side already read */
    size_t  prelen;
    int     ret;                /* what serve returns */
    int     closing;            /* the connection is ending: later errors are consequences */
    int     lng;                /* it ends on our decision, not the peer's: it may still be sending */
    char    buf[];              /* RBUF read, then WBUF write */
};

static void wake_writer(struct h2c *c)
{
    if (c->wf) em_unpark(c->wf);
}

static void fail(struct h2c *c, int e)
{
    if (!c->closing && !c->ret) c->ret = e;
}

/* Reader or writer is done: end the connection (the other fiber and every stream fiber are cancelled). */
static void end(struct h2c *c)
{
    c->closing = 1;
    em_scope_cancel(c->sc);
}

static void release(h2s *st)
{
    struct h2c *c = st->c;
    if (st->prev) st->prev->next = st->next; else c->streams = st->next;
    if (st->next) st->next->prev = st->prev;
    c->nstreams--;
    free(st);
}

/* ---- nghttp2 callbacks: never park ---- */

static h2s *stream_of(nghttp2_session *ses, int32_t id)
{
    return nghttp2_session_get_stream_user_data(ses, id);
}

static int is_request(const nghttp2_frame *f)
{
    return f->hd.type == NGHTTP2_HEADERS && f->headers.cat == NGHTTP2_HCAT_REQUEST;
}

static int on_begin_frame(nghttp2_session *ses, const nghttp2_frame_hd *hd, void *ud)
{
    (void)ses;
    struct h2c *c = ud;
    if (hd->type != NGHTTP2_CONTINUATION) c->cont = 0;
    else if (++c->cont > MAX_CONT) return NGHTTP2_ERR_CALLBACK_FAILURE; /* fatal: serve closes */
    return 0;
}

static int on_begin_headers(nghttp2_session *ses, const nghttp2_frame *f, void *ud)
{
    struct h2c *c = ud;
    if (!is_request(f)) return 0;
    if (c->nstreams >= c->maxstreams) { /* nghttp2 enforces it too, but only once the peer acked our SETTINGS */
        nghttp2_submit_rst_stream(ses, NGHTTP2_FLAG_NONE, f->hd.stream_id, NGHTTP2_REFUSED_STREAM);
        return 0;
    }
    h2s *st = malloc(sizeof *st + c->maxh * sizeof *st->hv + c->hcap);
    if (!st) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    *st = (h2s){ .h2 = 1, .done = 1, .id = f->hd.stream_id, .c = c, .next = c->streams };
    st->hv = (em_http_header *)(st + 1);
    st->hb = (char *)(st->hv + c->maxh);
    if (c->streams) c->streams->prev = st;
    c->streams = st;
    c->nstreams++;
    nghttp2_session_set_stream_user_data(ses, st->id, st);
    return 0;
}

static int on_header(nghttp2_session *ses, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                     const uint8_t *val, size_t vl, uint8_t flags, void *ud)
{
    (void)flags; (void)ud;
    if (!is_request(f)) return 0;
    h2s *st = stream_of(ses, f->hd.stream_id);
    if (!st) return 0; /* refused */
    struct h2c *c = st->c;
    const char *k = (const char *)name;
    size_t room = c->hcap - st->hn;
    if (vl > room || (name[0] != ':' && nl > room - vl)) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE; /* RST_STREAM */
    char *v = st->hb + st->hn;
    if (name[0] == ':') {
        if (nl == 7 && !memcmp(k, ":method", 7)) { st->req.method = v; st->req.method_len = vl; }
        else if (nl == 5 && !memcmp(k, ":path", 5)) { st->req.path = v; st->req.path_len = vl; }
        else if (nl == 10 && !memcmp(k, ":authority", 10)) { k = "host"; nl = 4; } /* so an h1 handler finds it */
        else return 0; /* :scheme */
    } else {
        memcpy(v, k, nl);
        k = v;
        v += nl;
        st->hn += nl;
    }
    memcpy(v, val, vl);
    st->hn += vl;
    if (k != (const char *)name) { /* a header, not :method / :path */
        if (st->nh == c->maxh) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        st->hv[st->nh++] = (em_http_header){ k, nl, v, vl };
    }
    return 0;
}

#define NV_STATIC(n, v) { (uint8_t *)(n), (uint8_t *)(v), sizeof n - 1, sizeof v - 1, \
                          NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE }

static int stream_fn(void *arg);

static int on_frame_recv(nghttp2_session *ses, const nghttp2_frame *f, void *ud)
{
    struct h2c *c = ud;
    if (!is_request(f)) return 0;
    h2s *st = stream_of(ses, f->hd.stream_id);
    if (!st) return 0;
    if (!(f->hd.flags & NGHTTP2_FLAG_END_STREAM) || !st->req.method || !st->req.path) {
        /* M1: a request body (or CONNECT) is not served. The response ends the stream; DATA that
         * still arrives is dropped by nghttp2, which keeps the connection's window open. */
        static const nghttp2_nv nv[] = { NV_STATIC(":status", "501"), NV_STATIC("content-length", "0") };
        nghttp2_submit_response(ses, st->id, nv, 2, NULL);
        return 0;
    }
    st->req.headers = st->hv;
    st->req.nheaders = st->nh;
    st->req.minor_version = 1;
    st->req.version = 20;
    st->head = st->req.method_len == 4 && !memcmp(st->req.method, "HEAD", 4);
    st->done = 0;
    if (em_spawn(c->sc, stream_fn, st) != 0) {
        st->done = 1;
        nghttp2_submit_rst_stream(ses, NGHTTP2_FLAG_NONE, st->id, NGHTTP2_REFUSED_STREAM);
    }
    return 0;
}

static int on_stream_close(nghttp2_session *ses, int32_t id, uint32_t code, void *ud)
{
    (void)ud;
    h2s *st = stream_of(ses, id);
    if (!st) return 0;
    st->closed = 1;
    st->err = code;
    if (st->waiter) em_unpark(st->waiter);
    if (st->done) release(st);
    return 0;
}

/* ---- the handler side ---- */

static ssize_t body_cb(nghttp2_session *ses, int32_t id, uint8_t *buf, size_t len, uint32_t *flags,
                       nghttp2_data_source *src, void *ud)
{
    (void)ses; (void)id; (void)ud;
    h2s *st = src->ptr;
    if (st->dead) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE; /* the handler is gone: its body may be too */
    size_t n = st->blen - st->boff;
    if (n > len) n = len;
    memcpy(buf, st->body + st->boff, n);
    st->boff += n;
    if (st->boff == st->blen) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static void nv_set(nghttp2_nv *nv, const void *n, size_t nl, const void *v, size_t vl)
{
    *nv = (nghttp2_nv){ (uint8_t *)n, (uint8_t *)v, nl, vl, NGHTTP2_NV_FLAG_NONE };
}

int em_http2_respond(em_http_conn *hc, int status, const em_http_header *h, size_t nh,
                     const void *body, size_t len)
{
    h2s *st = (h2s *)hc;
    struct h2c *c = st->c;
    if (st->responded) return -EALREADY;
    if (st->closed || st->dead) return -ECANCELED;
    if (status < 200 || status > 999) return -EINVAL; /* h2 has no interim responses here */
    if (nh > MAX_RESP_HDRS) return -EMSGSIZE;
    char code[3] = { (char)('0' + status / 100), (char)('0' + status / 10 % 10), (char)('0' + status % 10) };
    char cl[20], lc[LC_BYTES];
    size_t cll = (size_t)(em_http_put_u64(cl, len) - cl), ln = 0;
    nghttp2_nv nv[MAX_RESP_HDRS + 3];
    nv_set(&nv[0], ":status", 7, code, 3);
    nv_set(&nv[1], "date", 4, em_http_date_line() + 6, DATE_VAL);
    nv_set(&nv[2], "content-length", 14, cl, cll);
    for (size_t i = 0; i < nh; i++) {
        if (h[i].name_len > LC_BYTES - ln) return -EMSGSIZE;
        for (size_t k = 0; k < h[i].name_len; k++) {
            char ch = h[i].name[k];
            lc[ln + k] = ch >= 'A' && ch <= 'Z' ? (char)(ch | 0x20) : ch;
        }
        nv_set(&nv[3 + i], lc + ln, h[i].name_len, h[i].value, h[i].value_len);
        ln += h[i].name_len;
    }
    st->body = body; st->blen = len; st->boff = 0;
    nghttp2_data_provider prd = { .source.ptr = st, .read_callback = body_cb };
    int rc = nghttp2_submit_response(c->ses, st->id, nv, 3 + nh, len && !st->head ? &prd : NULL);
    if (rc) return rc == NGHTTP2_ERR_NOMEM ? -ENOMEM : -EINVAL;
    st->responded = 1;
    wake_writer(c);
    while (!st->closed) { /* body and nv are copied out as the writer goes: wait for the end */
        st->waiter = em_self();
        int r = em_park();
        st->waiter = NULL;
        if (r) { st->dead = 1; return r; }
    }
    return st->err ? -ECANCELED : 0;
}

ssize_t em_http2_read_body(em_http_conn *hc, void *buf, size_t len)
{
    (void)buf;
    if (((h2s *)hc)->responded) return -EALREADY;
    return len ? 0 : -EINVAL; /* M1: no request has a body */
}

static int stream_fn(void *arg)
{
    h2s *st = arg;
    struct h2c *c = st->c;
    c->h((em_http_conn *)st, &st->req, c->arg);
    if (!st->responded) em_http2_respond((em_http_conn *)st, 500, NULL, 0, NULL, 0);
    st->done = 1;
    if (st->closed) release(st);
    return 0; /* a nonzero return would cancel the connection's scope */
}

/* ---- the connection ---- */

static int xsend(struct h2c *c, const char *p, size_t len)
{
    return em_http_send_all(c->fd, c->tls, c->send_ns, p, len);
}

/* Everything nghttp2 has to say, as one send when it fits. >= 0: bytes sent; -errno. */
static ssize_t drain(struct h2c *c)
{
    char *wb = c->buf + RBUF;
    size_t wl = 0, total = 0;
    for (;;) {
        const uint8_t *p;
        ssize_t n = nghttp2_session_mem_send(c->ses, &p);
        if (n < 0) return -EPROTO;
        if (!n) break;
        total += (size_t)n;
        if ((size_t)n > WBUF - wl) { /* p is only valid until the next mem_send: use or copy it now */
            int r = wl ? xsend(c, wb, wl) : 0;
            wl = 0;
            if (!r && (size_t)n > WBUF) { r = xsend(c, (const char *)p, (size_t)n); n = 0; }
            if (r) return r;
        }
        memcpy(wb + wl, p, (size_t)n);
        wl += (size_t)n;
    }
    if (wl) {
        int r = xsend(c, wb, wl);
        if (r) return r;
    }
    return (ssize_t)total;
}

static int writer(void *arg)
{
    struct h2c *c = arg;
    for (;;) {
        ssize_t n = drain(c);
        if (n < 0) { fail(c, (int)n); break; }
        if (n) continue; /* sending yielded: more may have been queued meanwhile; ask again before parking */
        if (!nghttp2_session_want_read(c->ses) && !nghttp2_session_want_write(c->ses)) { c->lng = 1; break; }
        c->wf = em_self();
        int r = em_park();
        c->wf = NULL;
        if (r) break; /* cancelled: whoever did it is the reason */
    }
    end(c);
    return 0;
}

/* 1 = stop reading. */
static int feed(struct h2c *c, const void *p, size_t n)
{
    ssize_t r = nghttp2_session_mem_recv(c->ses, p, n);
    wake_writer(c); /* acks, window updates, a response */
    return r != (ssize_t)n || !nghttp2_session_want_read(c->ses);
}

static int reader(void *arg)
{
    struct h2c *c = arg;
    int stop = c->prelen && feed(c, c->pre, c->prelen);
    while (!stop) {
        ssize_t n = c->tls ? em_tls_recv(c->tls, c->buf, RBUF) : em_recv(c->fd, c->buf, RBUF);
        if (c->tls && n == -ECONNRESET) n = 0; /* TCP closed without close_notify: harmless, as in h1 */
        if (n <= 0) {
            if (n) fail(c, (int)n);
            break;
        }
        stop = feed(c, c->buf, (size_t)n);
    }
    c->lng |= stop;
    end(c);
    return 0;
}

int em_http2_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg,
                   const void *pre, size_t prelen)
{
    int64_t sto = !cfg || !cfg->send_timeout_ns ? DEF_SEND : cfg->send_timeout_ns;
    struct h2c *c = malloc(sizeof *c + RBUF + WBUF);
    if (!c) return -ENOMEM;
    memset(c, 0, sizeof *c);
    c->fd = fd; c->tls = tls; c->send_ns = sto > 0 ? sto : 0; c->h = h; c->arg = arg;
    c->pre = pre; c->prelen = prelen;
    c->hcap = cfg && cfg->max_header_bytes ? cfg->max_header_bytes : DEF_HCAP;
    c->maxh = cfg && cfg->max_headers ? cfg->max_headers : DEF_HDRS;
    c->maxstreams = cfg && cfg->max_concurrent_streams ? cfg->max_concurrent_streams : DEF_STREAMS;

    nghttp2_session_callbacks *cb;
    if (nghttp2_session_callbacks_new(&cb)) { free(c); return -ENOMEM; }
    nghttp2_session_callbacks_set_on_begin_frame_callback(cb, on_begin_frame);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, on_stream_close);
    int rc = nghttp2_session_server_new(&c->ses, cb, c);
    nghttp2_session_callbacks_del(cb);
    if (rc) { free(c); return -ENOMEM; }
    nghttp2_settings_entry iv[] = { { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, c->maxstreams },
                                    { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, (uint32_t)c->hcap } };
    nghttp2_submit_settings(c->ses, NGHTTP2_FLAG_NONE, iv, 2);

    em_scope sc;
    em_scope_open(&sc, 0);
    c->sc = &sc;
    if (em_spawn(&sc, writer, c) || em_spawn(&sc, reader, c)) { c->ret = -ENOMEM; em_scope_cancel(&sc); }
    em_scope_close(&sc);
    /* a GOAWAY queued by nghttp2 before the connection ended goes out; close() over bytes the peer is
     * still sending would RST and could destroy it, so linger as http.c does */
    if (!c->ret && drain(c) >= 0 && c->lng) em_http_linger(fd, c->buf, RBUF);
    nghttp2_session_del(c->ses);
    while (c->streams) release(c->streams);
    rc = c->ret;
    free(c);
    return rc;
}
