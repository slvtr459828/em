#ifndef EM_TEST_HTTP_UTIL_H
#define EM_TEST_HTTP_UTIL_H
/* Server side: test_handler answers "<method> <path>" (200, text/plain), with a few special
 * paths. Client side: a raw reader that splits the byte stream into responses.
 * Built with -DHTTP_TLS the same test runs over TLS: the server does em_tls_accept and passes the
 * em_tls to serve, the client does em_tls_connect and reads/writes through it (rd_send / rd_recv). */
#include <stdatomic.h>
#include <stdio.h>
#ifdef HTTP_TLS
#include <pthread.h>
#include "tls_util.h"
#else
#include "util.h"
#endif
#include "../include/em_http.h"

static em_http_cfg http_cfg; /* read when a connection starts; change it only between servers */
static char big_body[20001]; /* "/big": 20000 bytes + NUL for strlen in expect_resp */
static char k4_body[4097];    /* "/4k": 4096 bytes + NUL */
static char huge_body[16u << 20]; /* "/huge": 16 MiB of zeros, sent from here */
static _Atomic int http_done; /* serve calls that have returned (their value: http_rc) */
static _Atomic int http_rc;
#ifdef HTTP_TLS
static SSL_CTX *http_sctx, *http_cctx;
static pthread_once_t http_once = PTHREAD_ONCE_INIT;
static void http_tls_init(void) { tls_test_ctx(&http_sctx, &http_cctx); }
#endif

static int test_handler(em_http_conn *c, const em_http_req *r, void *arg)
{
    (void)arg;
    static const em_http_header ct = { "Content-Type", 12, "text/plain", 10 };
    char buf[512];
    if (r->path_len == 7 && !memcmp(r->path, "/noresp", 7)) return 0;
    if (r->path_len == 4 && !memcmp(r->path, "/err", 4)) return -EIO;
    if (r->path_len == 6 && !memcmp(r->path, "/okerr", 6)) {
        em_http_respond(c, 200, &ct, 1, "okerr", 5);
        return -EIO;
    }
    if (r->path_len == 4 && !memcmp(r->path, "/big", 4))
        return em_http_respond(c, 200, &ct, 1, big_body, sizeof big_body - 1);
    if (r->path_len == 5 && !memcmp(r->path, "/huge", 5))
        return em_http_respond(c, 200, &ct, 1, huge_body, sizeof huge_body);
    if (r->path_len == 3 && !memcmp(r->path, "/4k", 3))
        return em_http_respond(c, 200, &ct, 1, k4_body, sizeof k4_body - 1);
    if (r->path_len == 5 && !memcmp(r->path, "/echo", 5)) { /* the whole body, read in 1..7 byte steps */
        static char body[1 << 17];
        size_t n = 0, step = 1;
        for (;;) {
            size_t want = step < sizeof body - n ? step : sizeof body - n;
            ssize_t k = em_http_read_body(c, body + n, want);
            if (k < 0) return (int)k;
            if (!k) break;
            n += (size_t)k;
            step = step % 7 + 1;
        }
        return em_http_respond(c, 200, &ct, 1, body, n);
    }
    if (r->path_len == 7 && !memcmp(r->path, "/noread", 7))
        return em_http_respond(c, 200, &ct, 1, "noread", 6);
    assert(r->method_len + 1 + r->path_len < sizeof buf);
    memcpy(buf, r->method, r->method_len);
    buf[r->method_len] = ' ';
    memcpy(buf + r->method_len + 1, r->path, r->path_len);
    return em_http_respond(c, 200, &ct, 1, buf, r->method_len + 1 + r->path_len);
}

static int http_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    em_tls *t = NULL;
#ifdef HTTP_TLS
    if (em_tls_accept(&t, http_sctx, fd) < 0) { em_close(fd); return 0; }
#endif
    int rc = em_http_serve(fd, t, &http_cfg, test_handler, NULL);
    atomic_store(&http_rc, rc);
    atomic_fetch_add(&http_done, 1);
#ifdef HTTP_TLS
    em_tls_shutdown(t); /* the caller's close_notify; fails after a lingering close (write side is shut) */
    em_tls_free(t);
#endif
    em_close(fd);
    return 0;
}

struct hsrv { int lfd; em_scope *sc; };

static int http_acceptor(void *arg)
{
    struct hsrv *s = arg;
    for (;;) {
        int fd = em_accept(s->lfd);
        if (fd == -ECANCELED) return 0; /* listener closed */
        if (fd < 0) return fd;
        if (em_spawn(s->sc, http_conn, (void *)(intptr_t)fd) != 0) return -ENOMEM;
    }
}

/* Opens *sc (server scope), listens on 127.0.0.1:*port, starts the acceptor. */
static void http_start(struct hsrv *s, em_scope *sc, uint16_t *port)
{
#ifdef HTTP_TLS
    pthread_once(&http_once, http_tls_init);
#endif
    memset(big_body, 'b', sizeof big_body - 1);
    memset(k4_body, 'a', sizeof k4_body - 1);
    em_scope_open(sc, 0);
    s->sc = sc;
    s->lfd = listen_local(port);
    assert(em_spawn(sc, http_acceptor, s) == 0);
}

/* Every client connection must be closed before this (serve ends on EOF). */
static int http_stop(struct hsrv *s)
{
    em_close(s->lfd);
    return em_scope_close(s->sc);
}

/* ---- client ---- */

typedef struct { int fd; em_tls *t; size_t pos, len; char buf[1 << 16]; } rd_t;

static int rd_send(rd_t *r, const void *b, size_t n)
{
#ifdef HTTP_TLS
    return em_tls_send_all(r->t, b, n);
#else
    return em_send_all(r->fd, b, n);
#endif
}

/* 0 = end. Under TLS a TCP end without close_notify counts as one too: serve never sends it, and a
 * server that shut the write side (lingering close, send timeout) cannot. A response cut short is
 * still caught: the readers below treat an end inside a response as an error. */
static ssize_t rd_recv(rd_t *r, void *b, size_t n)
{
#ifdef HTTP_TLS
    ssize_t k = em_tls_recv(r->t, b, n);
    return k == -ECONNRESET ? 0 : k;
#else
    return em_recv(r->fd, b, n);
#endif
}

static void send_str(rd_t *r, const char *s)
{
    assert(rd_send(r, s, strlen(s)) == 0);
}

/* Next response from the stream. 0 = ok (head/body point into r->buf, valid until the next
 * call); 1 = EOF at a response boundary; -EIO = EOF or junk inside a response. */
static int rd_response(rd_t *r, const char **head, size_t *hlen, const char **body, size_t *blen)
{
    size_t hl = 0, cl = 0; /* hl = 0: head not complete yet */
    for (;;) {
        if (!hl) {
            char *e = memmem(r->buf + r->pos, r->len - r->pos, "\r\n\r\n", 4);
            if (e) {
                hl = (size_t)(e - (r->buf + r->pos)) + 4;
                char *p = memmem(r->buf + r->pos, hl, "\r\nContent-Length: ", 18);
                if (!p) return -EIO;
                cl = strtoull(p + 18, NULL, 10);
            }
        }
        if (hl && r->len - r->pos >= hl + cl) {
            *head = r->buf + r->pos; *hlen = hl;
            *body = *head + hl;      *blen = cl;
            r->pos += hl + cl;
            return 0;
        }
        if (r->pos) { /* compact; hl and cl are relative to pos, so they stay valid */
            memmove(r->buf, r->buf + r->pos, r->len - r->pos);
            r->len -= r->pos;
            r->pos = 0;
        }
        assert(r->len < sizeof r->buf);
        ssize_t n = rd_recv(r, r->buf + r->len, sizeof r->buf - r->len);
        if (n < 0) return (int)n;
        if (n == 0) return r->len ? -EIO : 1;
        r->len += (size_t)n;
    }
}

/* Next response must be `status_line` (prefix of the head, e.g. "HTTP/1.1 200 ") with exactly
 * `body`; conn = value of its Connection header, NULL = no Connection header. */
static void expect_resp(rd_t *r, const char *status_line, const char *body, const char *conn)
{
    const char *h = NULL, *b = NULL;
    size_t hl = 0, bl = 0;
    int rc = rd_response(r, &h, &hl, &b, &bl);
    if (rc) { fprintf(stderr, "expect %s: rd_response = %d\n", status_line, rc); abort(); }
    char hd[1024];
    assert(hl < sizeof hd);
    memcpy(hd, h, hl); hd[hl] = 0;
    if (strncmp(hd, status_line, strlen(status_line)) != 0 || bl != strlen(body) || memcmp(b, body, bl)) {
        fprintf(stderr, "expect %s / body %zu bytes, got:\n%s", status_line, strlen(body), hd);
        abort();
    }
    assert(strstr(hd, "\r\nDate: ") != NULL);
    if (conn) {
        char w[64];
        snprintf(w, sizeof w, "\r\nConnection: %s\r\n", conn);
        assert(strstr(hd, w) != NULL);
    } else {
        assert(strstr(hd, "\r\nConnection:") == NULL);
    }
}

static inline void expect_eof(rd_t *r)
{
    const char *h = NULL, *b = NULL;
    size_t hl = 0, bl = 0;
    assert(rd_response(r, &h, &hl, &b, &bl) == 1);
}

#define CONT "HTTP/1.1 100 Continue\r\n\r\n"

/* The next bytes must be "100 Continue" (it has no Content-Length, so rd_response cannot read it). */
static inline void expect_100(rd_t *r)
{
    size_t k = sizeof CONT - 1;
    while (r->len - r->pos < k) {
        if (r->pos) {
            memmove(r->buf, r->buf + r->pos, r->len - r->pos);
            r->len -= r->pos;
            r->pos = 0;
        }
        ssize_t n = rd_recv(r, r->buf + r->len, sizeof r->buf - r->len);
        assert(n > 0);
        r->len += (size_t)n;
    }
    assert(memcmp(r->buf + r->pos, CONT, k) == 0);
    r->pos += k;
}

/* rcvbuf > 0: SO_RCVBUF set before connect, so the window the server sees stays small and what it
 * sends piles up on its side while this client is not reading. */
static rd_t *rd_open_rcvbuf(uint16_t port, int rcvbuf)
{
    rd_t *r = calloc(1, sizeof *r);
    if (rcvbuf) {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        r->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(r->fd >= 0);
        assert(setsockopt(r->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) == 0);
        assert(connect(r->fd, (struct sockaddr *)&a, sizeof a) == 0); /* loopback: done in the kernel */
    } else {
        r->fd = connect_local(port);
    }
    assert(r->fd >= 0);
#ifdef HTTP_TLS
    assert(em_tls_connect(&r->t, http_cctx, r->fd, "localhost") == 0);
#endif
    return r;
}

static inline rd_t *rd_open(uint16_t port) { return rd_open_rcvbuf(port, 0); }

static void rd_close(rd_t *r)
{
#ifdef HTTP_TLS
    em_tls_free(r->t);
#endif
    em_close(r->fd);
    free(r);
}
#endif
