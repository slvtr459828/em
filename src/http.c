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

struct em_http_conn {
    int    fd;
    int    minor;      /* current request */
    int    keep;       /* current request: connection stays open after its response */
    int    responded;
    int    err;        /* sticky send error */
    char  *wbuf;
    size_t wlen;
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
    case 413: return "Content Too Large";
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

static int flush(em_http_conn *c)
{
    if (c->err) return c->err;
    if (c->wlen) {
        int r = em_send_all(c->fd, c->wbuf, c->wlen);
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
    int inl = len <= WBUF_BYTES - hub;  /* header and body share one buffer, one send */
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
    } else {
        c->wlen = (size_t)(p - c->wbuf);
        int r = flush(c);
        if (r < 0) return r;
        if ((r = em_send_all(c->fd, body, len)) < 0) return c->err = r;
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

/* 0: fine (no body, *keep set); else the status to answer before closing. */
static int inspect(const struct phr_header *hs, size_t nh, int minor, int *keep)
{
    int tokens = 0, te = 0, bad = 0;
    int64_t cl = -1;
    for (size_t i = 0; i < nh; i++) {
        const struct phr_header *h = &hs[i];
        if (!h->name) continue; /* obs-fold continuation line */
        if (IEQ(h->name, h->name_len, "connection")) {
            tokens |= conn_tokens(h->value, h->value_len);
        } else if (IEQ(h->name, h->name_len, "transfer-encoding")) {
            te = 1;
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
        }
    }
    if (bad || (te && cl >= 0)) return 400;  /* RFC 9112 6.3: smuggling shapes */
    if (te) return 501;
    if (cl > 0) return 413;
    *keep = minor ? !(tokens & 1) : (tokens & 3) == 2;
    return 0;
}

/* Answers an error, then the caller closes. The status line uses HTTP/1.minor. */
static int fail(em_http_conn *c, int status, int minor)
{
    c->minor = minor; c->keep = 0; c->responded = 0;
    int r = em_http_respond(c, status, NULL, 0, NULL, 0);
    return r < 0 ? r : flush(c);
}

int em_http_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg)
{
    if (tls) return -ENOTSUP;
    size_t rcap = cfg && cfg->max_header_bytes ? cfg->max_header_bytes : DEF_RBUF;
    size_t maxh = cfg && cfg->max_headers ? cfg->max_headers : DEF_HDRS;
    unsigned maxreq = cfg ? cfg->max_requests_per_conn : 0;
    struct phr_header *hs = malloc(maxh * sizeof *hs + WBUF_BYTES + rcap);
    if (!hs) return -ENOMEM;
    em_http_conn c = { .fd = fd, .wbuf = (char *)(hs + maxh) };
    char *rbuf = c.wbuf + WBUF_BYTES;
    size_t rpos = 0, rlen = 0, last = 0; /* unparsed bytes are rbuf[rpos, rlen) */
    unsigned nreq = 0;
    int ret = 0;

    for (;;) {
        /* 1. one complete request head in rbuf[rpos..] */
        const char *m, *path;
        size_t ml, pl, nh = maxh;
        int minor, pret = -2;
        size_t avail = rlen - rpos;
        if (avail) {
            pret = phr_parse_request(rbuf + rpos, avail, &m, &ml, &path, &pl, &minor, hs, &nh, last);
            if (pret == -1) { ret = fail(&c, 400, 1); break; }
        }
        if (pret == -2) {
            if (avail == rcap) { ret = fail(&c, 431, 1); break; }
            last = avail;
            /* About to wait: nothing queued may sit behind a read. */
            if ((ret = flush(&c)) < 0) break;
            if (rpos == rlen) rpos = rlen = 0;
            else if (rpos) { memmove(rbuf, rbuf + rpos, avail); rpos = 0; rlen = avail; }
            ssize_t n = em_recv(fd, rbuf + rlen, rcap - rlen);
            if (n < 0) { ret = (int)n; break; }
            if (n == 0) { ret = avail ? -ECONNRESET : 0; break; }
            rlen += (size_t)n;
            continue;
        }
        last = 0;

        /* 2. no body in M1; connection semantics */
        int keep, st = inspect(hs, nh, minor, &keep);
        if (st) { ret = fail(&c, st, minor); break; }
        if (maxreq && ++nreq >= maxreq) keep = 0;

        /* 3. the handler */
        em_http_req req = { .method = m, .path = path, .method_len = ml, .path_len = pl,
                            .minor_version = minor, .headers = (const em_http_header *)hs,
                            .nheaders = nh };
        c.minor = minor; c.keep = keep; c.responded = 0;
        int hr = h(&c, &req, arg);
        if (c.err) { ret = c.err; break; }
        if (hr || !c.responded) {
            /* answered, then failed: that answer stands, no second one; then close */
            ret = c.responded ? flush(&c) : fail(&c, 500, minor);
            if (ret >= 0) ret = hr ? hr : -EPROTO;
            break;
        }
        rpos += (size_t)pret;
        if (!keep) break;
    }
    if (ret >= 0) { int r = flush(&c); if (r < 0) ret = r; } /* the last response(s) */
    free(hs);
    return ret;
}
