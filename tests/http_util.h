#ifndef EM_TEST_HTTP_UTIL_H
#define EM_TEST_HTTP_UTIL_H
/* Server side: test_handler answers "<method> <path>" (200, text/plain), with a few special
 * paths. Client side: a raw reader that splits the byte stream into responses. */
#include <stdio.h>
#include "util.h"
#include "../include/em_http.h"

static em_http_cfg http_cfg; /* read when a connection starts; change it only between servers */
static char big_body[20001]; /* "/big": 20000 bytes + NUL for strlen in expect_resp */
static char k4_body[4097];    /* "/4k": 4096 bytes + NUL */

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
    if (r->path_len == 3 && !memcmp(r->path, "/4k", 3))
        return em_http_respond(c, 200, &ct, 1, k4_body, sizeof k4_body - 1);
    assert(r->method_len + 1 + r->path_len < sizeof buf);
    memcpy(buf, r->method, r->method_len);
    buf[r->method_len] = ' ';
    memcpy(buf + r->method_len + 1, r->path, r->path_len);
    return em_http_respond(c, 200, &ct, 1, buf, r->method_len + 1 + r->path_len);
}

static int http_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    em_http_serve(fd, NULL, &http_cfg, test_handler, NULL);
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

typedef struct { int fd; size_t pos, len; char buf[1 << 16]; } rd_t;

static void send_str(int fd, const char *s)
{
    assert(em_send_all(fd, s, strlen(s)) == 0);
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
        ssize_t n = em_recv(r->fd, r->buf + r->len, sizeof r->buf - r->len);
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

static rd_t *rd_open(uint16_t port)
{
    rd_t *r = calloc(1, sizeof *r);
    r->fd = connect_local(port);
    assert(r->fd >= 0);
    return r;
}

static void rd_close(rd_t *r)
{
    em_close(r->fd);
    free(r);
}
#endif
