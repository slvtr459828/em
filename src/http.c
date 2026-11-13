/* HTTP/1.x server: see include/em_http.h. One malloc per connection holds the header array,
 * the write buffer and the read buffer; nothing is allocated per request. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "em.h"
#include "em_http.h"
#include "picohttpparser.h"

#define WBUF_BYTES 8192u
#define DEF_RBUF   8192u
#define DEF_HDRS   64u
/* Upper bound of everything respond writes besides user headers and body: status line with
 * reason (<= 39), Date (37), Content-Length (38), Connection (24), final CRLF (2). */
#define FIXED_HDR  160u
/* Chunk-decode window: the one fixed region this file adds to the per-connection malloc. The head
 * must stay put while the handler runs, so raw chunked bytes cannot be decoded in rbuf (its tail may
 * be full); phr_decode_chunked rewrites its buffer in place, so raw bytes are received into this
 * window and decoded there. Never used by requests without a chunked body. */
#define CWIN_BYTES 8192u
#define DEF_HDR_TO 30000000000ll
#define DEF_IDLE   60000000000ll
#define DRAIN_MAX  65536u   /* an unread Content-Length remainder up to this is drained and the connection kept */
#define DEF_SEND   60000000000ll
#define TLS_CHUNK  65536u   /* em_tls_send_all reports nothing until it is done: bound what one deadline covers */
/* Lingering close (see linger): a fiber held by a peer that may be hostile, so stricter than nginx
 * (lingering_timeout 5 s, lingering_time 30 s). 2 s covers a client reacting to the response over a
 * slow link; 1 MiB is the in-flight data of one RTT at a few hundred Mbit/s, past that it is a flood. */
#define LINGER_NS  2000000000ll
#define LINGER_MAX (1u << 20)

/* em_tls.h pulls in OpenSSL, which a plain-HTTP user need not have, so the two calls made here are
 * declared again, weak: tls != NULL means the caller got it from em_tls_accept and so linked
 * libem_tls, which binds them; with tls == NULL they are never reached. Keep in step with em_tls.h. */
ssize_t em_tls_recv(em_tls *t, void *buf, size_t len) __attribute__((weak));
int     em_tls_send_all(em_tls *t, const void *buf, size_t len) __attribute__((weak));

struct em_http_conn {
    int     fd;
    em_tls *tls;        /* NULL = plain */
    int64_t send_ns;    /* 0 = none */
    int     minor;      /* current request */
    int     keep;       /* current request: connection stays open after its response */
    int     responded;
    int     err;        /* sticky send error */
    int     head;       /* current request is HEAD: respond sends no body */
    char   *wbuf;
    size_t  wlen;
    /* read side. Request k's head is rbuf[hpos, bpos); its body bytes already received and not yet
     * read are rbuf[bpos, bend) (Content-Length) or the raw bytes rbuf[bpos, rlen) (chunked). */
    char   *rbuf;
    size_t  rcap, rlen, bpos, bend;
    int64_t idle_ns;    /* 0 = none */
    uint64_t max_body;
    int64_t body_left;  /* Content-Length: bytes not yet read by the handler */
    int     chunked;
    int     expect;     /* 100 Continue still owed (HTTP/1.1 + Expect: 100-continue + a body) */
    int     rerr;       /* first read_body error of this request; the connection closes after it */
    int     rown;       /* rerr is our own idle timeout, not the caller's scope deadline */
    int     cdone;      /* chunked: terminating chunk and trailers seen */
    char   *cw;         /* chunk window */
    size_t  cw_pos, cw_len, cw_left; /* decoded bytes cw[cw_pos, cw_len); then cw_left bytes of the next request */
    uint64_t cdec;      /* chunked: decoded bytes so far */
    struct phr_chunked_decoder dec;
};

/* The user sees phr_header through em_http_header: same layout, enforced here. */
_Static_assert(sizeof(em_http_header) == sizeof(struct phr_header) &&
               offsetof(em_http_header, name) == offsetof(struct phr_header, name) &&
               offsetof(em_http_header, name_len) == offsetof(struct phr_header, name_len) &&
               offsetof(em_http_header, value) == offsetof(struct phr_header, value) &&
               offsetof(em_http_header, value_len) == offsetof(struct phr_header, value_len),
               "em_http_header must match phr_header");

/* ---- Date: "Date: Thu, 12 Nov 2026 12:00:00 GMT\r\n", rebuilt when the second changes. A fiber
 * never leaves its worker thread, so a __thread cache is private to the worker. ---- */
static __thread struct { time_t sec; char line[40]; } date_cache;
#define DATE_LEN 37u

static const char *date_line(void)
{
    time_t now = time(NULL);
    if (now != date_cache.sec || !date_cache.line[0]) {
        static const char wd[] = "SunMonTueWedThuFriSat";
        static const char mo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        struct tm tm;
        gmtime_r(&now, &tm);
        char *p = date_cache.line;
        memcpy(p, "Date: ", 6); p += 6;
        memcpy(p, wd + tm.tm_wday * 3, 3); p += 3;
        *p++ = ','; *p++ = ' ';
        *p++ = (char)('0' + tm.tm_mday / 10); *p++ = (char)('0' + tm.tm_mday % 10); *p++ = ' ';
        memcpy(p, mo + tm.tm_mon * 3, 3); p += 3; *p++ = ' ';
        int y = tm.tm_year + 1900;
        *p++ = (char)('0' + y / 1000 % 10); *p++ = (char)('0' + y / 100 % 10);
        *p++ = (char)('0' + y / 10 % 10);   *p++ = (char)('0' + y % 10); *p++ = ' ';
        *p++ = (char)('0' + tm.tm_hour / 10); *p++ = (char)('0' + tm.tm_hour % 10); *p++ = ':';
        *p++ = (char)('0' + tm.tm_min / 10);  *p++ = (char)('0' + tm.tm_min % 10);  *p++ = ':';
        *p++ = (char)('0' + tm.tm_sec / 10);  *p++ = (char)('0' + tm.tm_sec % 10);
        memcpy(p, " GMT\r\n", 7);   /* with the NUL */
        date_cache.sec = now;
    }
    return date_cache.line;
}

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown"; /* the phrase is informational (RFC 9112 4) */
    }
}

static char *put_u64(char *p, uint64_t v)
{
    char t[20];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

/* Every send serve makes. The deadline is per call that makes progress, not per response: a
 * slow reader that keeps taking bytes is never cut, one that takes none for send_ns is (-ETIMEDOUT,
 * ours or the caller's scope: both end the connection the same way). TLS goes in TLS_CHUNK pieces. */
static int send_all(em_http_conn *c, const char *p, size_t len)
{
    while (len) {
        size_t n = c->tls && len > TLS_CHUNK ? TLS_CHUNK : len;
        em_scope s;
        if (c->send_ns) em_scope_open(&s, em_now() + c->send_ns);
        ssize_t r = c->tls ? em_tls_send_all(c->tls, p, n) : em_send(c->fd, p, n);
        if (c->send_ns) em_scope_close(&s); /* no children: nothing to wait for */
        if (r < 0) return (int)r;
        if (c->tls) r = (ssize_t)n;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static int flush(em_http_conn *c)
{
    if (c->err) return c->err;
    if (c->wlen) {
        int r = send_all(c, c->wbuf, c->wlen);
        c->wlen = 0;
        if (r < 0) return c->err = r;
    }
    return 0;
}

int em_http_respond(em_http_conn *c, int status, const em_http_header *h, size_t nh,
                    const void *body, size_t len)
{
    if (c->err) return c->err;
    if (c->responded) return -EALREADY;
    if (status < 100 || status > 999) return -EINVAL;
    size_t hub = FIXED_HDR;
    for (size_t i = 0; i < nh; i++) hub += h[i].name_len + h[i].value_len + 4;
    if (hub > WBUF_BYTES) return -EMSGSIZE;
    int inl = !c->head && len <= WBUF_BYTES - hub;  /* header and body share one buffer, one send */
    /* Decide Connection before the header is written: a body the handler left unread that we
     * cannot afford to drain ends the connection with this response. */
    if (c->keep && (c->chunked || c->body_left || c->expect || c->rerr) &&
        (c->rerr || c->expect || (c->chunked ? !c->cdone : c->body_left > (int64_t)DRAIN_MAX)))
        c->keep = 0;
    if (WBUF_BYTES - c->wlen < (inl ? hub + len : hub)) {
        int r = flush(c);
        if (r < 0) return r;
    }
    char *p = c->wbuf + c->wlen;
    memcpy(p, "HTTP/1.x ", 9);
    p[7] = (char)('0' + c->minor);
    p += 9;
    *p++ = (char)('0' + status / 100); *p++ = (char)('0' + status / 10 % 10);
    *p++ = (char)('0' + status % 10);  *p++ = ' ';
    const char *rs = reason(status);
    size_t rl = strlen(rs);
    memcpy(p, rs, rl); p += rl;
    memcpy(p, "\r\n", 2); p += 2;
    memcpy(p, date_line(), DATE_LEN); p += DATE_LEN;
    for (size_t i = 0; i < nh; i++) {
        memcpy(p, h[i].name, h[i].name_len); p += h[i].name_len;
        *p++ = ':'; *p++ = ' ';
        memcpy(p, h[i].value, h[i].value_len); p += h[i].value_len;
        *p++ = '\r'; *p++ = '\n';
    }
    memcpy(p, "Content-Length: ", 16); p += 16;
    p = put_u64(p, len);
    *p++ = '\r'; *p++ = '\n';
    if (!c->keep) { memcpy(p, "Connection: close\r\n", 19); p += 19; }
    else if (c->minor == 0) { memcpy(p, "Connection: keep-alive\r\n", 24); p += 24; }
    *p++ = '\r'; *p++ = '\n';
    if (inl) {
        if (len) memcpy(p, body, len);
        c->wlen = (size_t)(p - c->wbuf) + len;
    } else if (c->head) {
        c->wlen = (size_t)(p - c->wbuf);
    } else {
        c->wlen = (size_t)(p - c->wbuf);
        int r = flush(c);
        if (r < 0) return r;
        if ((r = send_all(c, body, len)) < 0) return c->err = r;
    }
    c->responded = 1;
    return 0;
}

/* ---- request inspection ---- */

static int ieq(const char *a, size_t n, const char *lit, size_t ln) /* lit is lower case */
{
    if (n != ln) return 0;
    for (size_t i = 0; i < n; i++)
        if ((a[i] | 0x20) != lit[i]) return 0;
    return 1;
}
#define IEQ(a, n, lit) ieq(a, n, lit, sizeof lit - 1)

/* A field value without its optional white space. */
static void trim(const char **v, size_t *n)
{
    while (*n && (**v == ' ' || **v == '\t')) { (*v)++; (*n)--; }
    while (*n && ((*v)[*n - 1] == ' ' || (*v)[*n - 1] == '\t')) (*n)--;
}

/* Connection: comma separated tokens. bit 0 = close, bit 1 = keep-alive. */
static int conn_tokens(const char *v, size_t n)
{
    int f = 0;
    for (size_t i = 0; i < n; ) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < n && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (IEQ(v + s, i - s, "close")) f |= 1;
        else if (IEQ(v + s, i - s, "keep-alive")) f |= 2;
    }
    return f;
}

/* 0: fine, *keep / *cl (0 none, -1 chunked, else the length) / *expect set; else the status to
 * answer before closing. */
static int inspect(const struct phr_header *hs, size_t nh, int minor, uint64_t max_body,
                   int *keep, int64_t *cl_out, int *expect)
{
    int tokens = 0, nte = 0, te_chunked = 0, bad = 0, xbad = 0, x100 = 0;
    int64_t cl = -1;
    for (size_t i = 0; i < nh; i++) {
        const struct phr_header *h = &hs[i];
        if (!h->name) { bad = 1; continue; } /* obs-fold continuation line: refused (RFC 9112 5.2) */
        if (IEQ(h->name, h->name_len, "connection")) {
            tokens |= conn_tokens(h->value, h->value_len);
        } else if (IEQ(h->name, h->name_len, "transfer-encoding")) {
            const char *v = h->value; size_t n = h->value_len;
            trim(&v, &n);
            nte++;
            te_chunked = IEQ(v, n, "chunked");
        } else if (IEQ(h->name, h->name_len, "content-length")) {
            int64_t v = 0;
            if (!h->value_len) bad = 1;
            for (size_t k = 0; k < h->value_len; k++) {
                unsigned d = (unsigned char)h->value[k] - '0';
                if (d > 9) { bad = 1; break; }
                if (v < ((int64_t)1 << 56)) v = v * 10 + d; /* saturates, still > 0 */
            }
            if (cl >= 0 && cl != v) bad = 1; /* repeated with another value */
            cl = v;
        } else if (IEQ(h->name, h->name_len, "expect")) {
            const char *v = h->value; size_t n = h->value_len;
            trim(&v, &n);
            if (IEQ(v, n, "100-continue")) x100 = 1; else xbad = 1;
        }
    }
    if (bad || (nte && cl >= 0)) return 400;  /* RFC 9112 6.3: smuggling shapes */
    if (nte && (nte > 1 || !te_chunked)) return 501;
    if (nte && minor == 0) return 400;        /* chunked does not exist in HTTP/1.0 */
    if (xbad) return 417;
    if (cl > 0 && (uint64_t)cl > max_body) return 413;
    *cl_out = nte ? -1 : cl > 0 ? cl : 0;
    *expect = x100 && minor && *cl_out;
    *keep = minor ? !(tokens & 1) : (tokens & 3) == 2;
    return 0;
}

/* Answers an error, then the caller closes. The status line uses HTTP/1.minor. */
static int fail(em_http_conn *c, int status, int minor)
{
    c->minor = minor; c->keep = 0; c->responded = 0; c->head = 0;
    int r = em_http_respond(c, status, NULL, 0, NULL, 0);
    return r < 0 ? r : flush(c);
}

/* ---- waiting and the request body ---- */

/* em_recv that gives up at the absolute deadline dl (0 = none) through a scope. *own = the error
 * is -ETIMEDOUT and it is ours: the effective deadline is the min of dl and the caller's, so ours
 * fired only if it is strictly earlier. The caller's -ETIMEDOUT / -ECANCELED stay theirs. */
static ssize_t recv_some(const em_http_conn *c, void *buf, size_t len)
{
    return c->tls ? em_tls_recv(c->tls, buf, len) : em_recv(c->fd, buf, len);
}

static ssize_t wait_recv(const em_http_conn *c, void *buf, size_t len, int64_t dl, int *own)
{
    *own = 0;
    if (!dl) return recv_some(c, buf, len);
    em_scope s;
    em_scope_open(&s, dl);
    ssize_t n = recv_some(c, buf, len);
    if (n == -ETIMEDOUT) *own = !s.parent->deadline || dl < s.parent->deadline;
    em_scope_close(&s); /* no children: nothing to wait for */
    return n;
}

static int64_t idle_deadline(const em_http_conn *c)
{
    return c->idle_ns ? em_now() + c->idle_ns : 0;
}

static ssize_t body_fail(em_http_conn *c, ssize_t e, int own)
{
    c->rerr = (int)e;
    c->rown = own;
    return e;
}

ssize_t em_http_read_body(em_http_conn *c, void *buf, size_t len)
{
    if (c->err) return c->err;
    if (c->responded) return -EALREADY;
    if (c->rerr) return c->rerr;
    if (!len) return -EINVAL;
    if (c->expect) { /* earlier pipelined responses go out first, in the same send */
        static const char k[] = "HTTP/1.1 100 Continue\r\n\r\n";
        c->expect = 0;
        if (WBUF_BYTES - c->wlen < sizeof k - 1) {
            int r = flush(c);
            if (r < 0) return r;
        }
        memcpy(c->wbuf + c->wlen, k, sizeof k - 1);
        c->wlen += sizeof k - 1;
        int r = flush(c);
        if (r < 0) return r;
    }
    if (!c->chunked) {
        if (!c->body_left) return 0;
        size_t want = (uint64_t)c->body_left < len ? (size_t)c->body_left : len;
        size_t b = c->bend - c->bpos; /* bytes that came with the head */
        if (b) {
            size_t n = b < want ? b : want;
            memcpy(buf, c->rbuf + c->bpos, n);
            c->bpos += n;
            c->body_left -= (int64_t)n;
            return (ssize_t)n;
        }
        int own;
        ssize_t n = wait_recv(c, buf, want, idle_deadline(c), &own); /* straight into buf, never past the body */
        if (n <= 0) return body_fail(c, n ? n : -ECONNRESET, own);
        c->body_left -= n;
        return n;
    }
    for (;;) {
        if (c->cw_pos < c->cw_len) {
            size_t n = c->cw_len - c->cw_pos < len ? c->cw_len - c->cw_pos : len;
            memcpy(buf, c->cw + c->cw_pos, n);
            c->cw_pos += n;
            return (ssize_t)n;
        }
        if (c->cdone) return 0;
        /* one raw batch into the window: the bytes that came with the head first, then the socket.
         * batch <= rcap so that what the decoder leaves of the next request fits rbuf again. */
        size_t batch = c->rcap < CWIN_BYTES ? c->rcap : CWIN_BYTES, n;
        if (c->bpos < c->rlen) {
            n = c->rlen - c->bpos < batch ? c->rlen - c->bpos : batch;
            memcpy(c->cw, c->rbuf + c->bpos, n);
            c->bpos += n;
        } else {
            int own;
            ssize_t r = wait_recv(c, c->cw, batch, idle_deadline(c), &own);
            if (r <= 0) return body_fail(c, r ? r : -ECONNRESET, own);
            n = (size_t)r;
        }
        size_t sz = n;
        ssize_t r = phr_decode_chunked(&c->dec, c->cw, &sz);
        if (r == -1) return body_fail(c, -EPROTO, 0);
        c->cdec += sz;
        if (c->cdec > c->max_body) return body_fail(c, -EMSGSIZE, 0);
        if (r >= 0) { c->cdone = 1; c->cw_left = (size_t)r; }
        c->cw_pos = 0;
        c->cw_len = sz;
    }
}

/* After the handler, connection kept: skip what it did not read and put the next request's bytes
 * at rbuf[0, rlen). respond made sure a kept connection has a finished chunked body or a
 * Content-Length remainder <= DRAIN_MAX. 0, 1 = close silently (our idle timeout), or -errno. */
static int finish_body(em_http_conn *c, size_t *rpos)
{
    if (c->chunked) {
        size_t rest = c->rlen - c->bpos; /* not yet fed to the decoder */
        memmove(c->rbuf + c->cw_left, c->rbuf + c->bpos, rest);
        memcpy(c->rbuf, c->cw + c->cw_len, c->cw_left);
        c->rlen = c->cw_left + rest;
        *rpos = 0;
        return 0;
    }
    size_t b = c->bend - c->bpos;
    size_t t = (uint64_t)c->body_left < b ? (size_t)c->body_left : b;
    c->bpos += t;
    int64_t skip = c->body_left - (int64_t)t;
    *rpos = c->bpos;
    if (!skip) return 0;
    /* skip > 0: the body went past what was buffered, so nothing else is */
    int r = flush(c); /* the response must not wait behind a slow body */
    if (r < 0) return r;
    c->rlen = *rpos = 0; /* the head is dead: rbuf is scratch */
    while (skip) {
        int own;
        ssize_t n = wait_recv(c, c->rbuf, (uint64_t)skip < c->rcap ? (size_t)skip : c->rcap,
                              idle_deadline(c), &own);
        if (n < 0) return own ? 1 : (int)n;
        if (!n) return -ECONNRESET;
        skip -= n;
    }
    return 0;
}

/* The peer may still be sending request bytes that nobody will read (a body the handler left
 * unread, or a chunked one that broke), as opposed to being gone or stalled. */
static int unread(const em_http_conn *c)
{
    if (c->rerr && c->rerr != -EMSGSIZE && c->rerr != -EPROTO) return 0;
    return c->chunked ? !c->cdone : c->expect || c->body_left > (int64_t)(c->bend - c->bpos);
}

/* Lingering close, after the final response was flushed and only when unread(): close() with
 * unread bytes in the socket makes the kernel send RST, which can destroy the response before the
 * client reads it. So send FIN after the response (SHUT_WR), then discard what the peer still sends
 * until it ends, LINGER_NS or LINGER_MAX. Raw bytes on the fd, also under TLS (records are not
 * parsed, no close_notify is sent: the write side is already shut). The caller still closes. */
static void linger(em_http_conn *c)
{
    if (em_shutdown(c->fd, SHUT_WR) < 0) return;
    em_scope s;
    em_scope_open(&s, em_now() + LINGER_NS); /* the caller's cancel and deadline still end it earlier */
    for (size_t left = LINGER_MAX; left; ) {
        ssize_t n = em_recv(c->fd, c->cw, left < CWIN_BYTES ? left : CWIN_BYTES); /* cw is free here */
        if (n <= 0) break;
        left -= (size_t)n;
    }
    em_scope_close(&s);
}

int em_http_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg)
{    size_t rcap = cfg && cfg->max_header_bytes ? cfg->max_header_bytes : DEF_RBUF;
    size_t maxh = cfg && cfg->max_headers ? cfg->max_headers : DEF_HDRS;
    unsigned maxreq = cfg ? cfg->max_requests_per_conn : 0;
    int64_t hto = !cfg || !cfg->header_timeout_ns ? DEF_HDR_TO : cfg->header_timeout_ns;
    int64_t ito = !cfg || !cfg->idle_timeout_ns ? DEF_IDLE : cfg->idle_timeout_ns;
    int64_t sto = !cfg || !cfg->send_timeout_ns ? DEF_SEND : cfg->send_timeout_ns;
    struct phr_header *hs = malloc(maxh * sizeof *hs + WBUF_BYTES + CWIN_BYTES + rcap);
    if (!hs) return -ENOMEM;
    em_http_conn c = { .fd = fd, .tls = tls, .send_ns = sto > 0 ? sto : 0, .wbuf = (char *)(hs + maxh), .rcap = rcap,
                       .idle_ns = ito > 0 ? ito : 0,
                       .max_body = cfg && cfg->max_body_bytes ? cfg->max_body_bytes : 1u << 20 };
    c.cw = c.wbuf + WBUF_BYTES;
    char *rbuf = c.rbuf = c.cw + CWIN_BYTES;
    size_t rpos = 0, last = 0; /* unparsed bytes are rbuf[rpos, c.rlen) */
    int64_t hdr_dl = 0;        /* deadline of the head being received, 0 = not started or none */
    unsigned nreq = 0;
    int ret = 0;
    int lng = 0;               /* close lingers: the peer may still be sending */

    for (;;) {
        /* 1. one complete request head in rbuf[rpos..] */
        const char *m, *path;
        size_t ml, pl, nh = maxh;
        int minor, pret = -2;
        size_t avail = c.rlen - rpos;
        if (avail) {
            pret = phr_parse_request(rbuf + rpos, avail, &m, &ml, &path, &pl, &minor, hs, &nh, last);
            if (pret == -1) { ret = fail(&c, 400, 1); lng = 1; break; }
        }
        if (pret == -2) {
            if (avail == rcap) { ret = fail(&c, 431, 1); lng = 1; break; }
            last = avail;
            /* About to wait: nothing queued may sit behind a read. */
            if ((ret = flush(&c)) < 0) break;
            if (rpos == c.rlen) rpos = c.rlen = 0;
            else if (rpos) { memmove(rbuf, rbuf + rpos, avail); rpos = 0; c.rlen = avail; }
            int64_t dl = 0;
            if (!avail) dl = idle_deadline(&c);  /* waiting for a request to start */
            else if (hto > 0) {                  /* a head is partly in: its clock runs from now at the latest */
                if (!hdr_dl) hdr_dl = em_now() + hto;
                dl = hdr_dl;
            }
            int own;
            ssize_t n = wait_recv(&c, rbuf + c.rlen, rcap - c.rlen, dl, &own);
            if (n < 0) {
                /* TCP closed without close_notify between requests: harmless (HTTP frames its own
                 * messages), and em_tls_recv cannot tell it from a reset */
                if (tls && n == -ECONNRESET && !avail) n = 0;
                else { ret = !own ? (int)n : avail ? fail(&c, 408, 1) : 0; break; }
            }
            if (n == 0) { ret = avail ? -ECONNRESET : 0; break; }
            c.rlen += (size_t)n;
            continue;
        }
        last = 0;
        hdr_dl = 0;

        /* 2. body framing and connection semantics */
        int keep, expect;
        int64_t cl;
        int st = inspect(hs, nh, minor, c.max_body, &keep, &cl, &expect);
        if (st) { ret = fail(&c, st, minor); lng = 1; break; }
        if (maxreq && ++nreq >= maxreq) keep = 0;

        /* 3. the handler */
        em_http_req req = { .method = m, .path = path, .method_len = ml, .path_len = pl,
                            .minor_version = minor, .headers = (const em_http_header *)hs,
                            .nheaders = nh, .content_length = cl };
        c.minor = minor; c.keep = keep; c.responded = 0;
        c.head = ml == 4 && !memcmp(m, "HEAD", 4);
        c.expect = expect; c.rerr = 0; c.chunked = cl < 0; c.cdone = 0;
        c.body_left = cl > 0 ? cl : 0;
        c.bpos = rpos + (size_t)pret;
        c.bend = cl > 0 && (uint64_t)cl < c.rlen - c.bpos ? c.bpos + (size_t)cl : c.rlen;
        if (cl < 0) {
            memset(&c.dec, 0, sizeof c.dec);
            c.dec.consume_trailer = 1;
            c.cw_pos = c.cw_len = c.cw_left = 0;
            c.cdec = 0;
        } else if (cl == 0) {
            c.bend = c.bpos;
        }
        int hr = h(&c, &req, arg);
        if (c.err) { ret = c.err; break; }
        if (hr || !c.responded) {
            /* answered, then failed: that answer stands, no second one; then close */
            int stt = 0;
            lng = unread(&c);
            if (c.responded) ret = flush(&c);
            else if (c.rerr) { /* a body error the handler left unanswered */
                stt = c.rerr == -EMSGSIZE ? 413 : c.rerr == -EPROTO ? 400 : c.rerr == -ETIMEDOUT && c.rown ? 408 : 0;
                ret = stt ? fail(&c, stt, minor) : 0;
            } else ret = fail(&c, 500, minor);
            if (ret >= 0) ret = hr ? hr : !c.rerr ? -EPROTO : stt ? 0 : c.rerr;
            break;
        }
        /* bytes past this request already buffered: the client is pipelining and may still send */
        if (!c.keep) { lng = unread(&c) || (!c.chunked && c.rlen > c.bend); break; }
        int fb = finish_body(&c, &rpos);
        if (fb) { ret = fb < 0 ? fb : 0; break; }
    }
    if (ret >= 0) { int r = flush(&c); if (r < 0) ret = r; } /* the last response(s) */
    if (lng && !c.err) linger(&c);
    free(hs);
    return ret;
}
