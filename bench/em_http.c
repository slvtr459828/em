/* HTTP server for the bench: argv[1] = port, argv[2] = workers (default 1), argv[3] = "tls" for
 * HTTP over em_tls. Same shape as em_echo / em_tls_echo: each worker listens with SO_REUSEPORT,
 * one fiber per connection. TLS: the certificate is generated at startup (tests/tls_util.h); the
 * bench client accepts any certificate.
 * GET /   -> 200 text/plain "Hello, World!" (13 B)
 * GET /4k -> 200 text/plain 4096 x 'a'
 * POST /echo -> 200, the request body back (tests/http_curl.sh; not a bench cell)
 * else    -> 404 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/em.h"
#include "../include/em_http.h"
#include "../tests/tls_util.h"

static struct sockaddr_in addr;
static char body4k[4096];
static SSL_CTX *sctx; /* NULL = plain */

static const em_http_cfg cfg = { .max_body_bytes = 16u << 20 }; /* /echo takes up to 16 MiB */

/* Reads the whole body into a malloc'd buffer (grown as needed: the body may be chunked) and
 * answers with it. A read error returns to serve, which answers 413/400/408 itself. */
static int echo(em_http_conn *c, const em_http_req *r)
{
    static const em_http_header bin = { "Content-Type", 12, "application/octet-stream", 24 };
    size_t cap = r->content_length > 0 ? (size_t)r->content_length : 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -ENOMEM;
    for (;;) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); return -ENOMEM; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = em_http_read_body(c, buf + len, cap - len);
        if (n < 0) { free(buf); return (int)n; }
        if (n == 0) break;
        len += (size_t)n;
    }
    int rc = em_http_respond(c, 200, &bin, 1, buf, len);
    free(buf);
    return rc;
}

static int handle(em_http_conn *c, const em_http_req *r, void *arg)
{
    (void)arg;
    static const em_http_header text = { "Content-Type", 12, "text/plain", 10 };
    if (r->path_len == 1 && r->path[0] == '/') return em_http_respond(c, 200, &text, 1, "Hello, World!", 13);
    if (r->path_len == 3 && memcmp(r->path, "/4k", 3) == 0) return em_http_respond(c, 200, &text, 1, body4k, sizeof body4k);
    if (r->path_len == 5 && memcmp(r->path, "/echo", 5) == 0 && r->method_len == 4 && memcmp(r->method, "POST", 4) == 0)
        return echo(c, r);
    return em_http_respond(c, 404, &text, 1, "Not Found\n", 10);
}

static int conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    if (!sctx) {
        em_http_serve(fd, NULL, &cfg, handle, NULL);
    } else {
        /* the server writes the session tickets, then the response: without NODELAY Nagle holds the
         * response behind the unacked tickets (as in em_tls_echo.c) */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
        em_tls *t;
        if (em_tls_accept(&t, sctx, fd) == 0) {
            em_http_serve(fd, t, &cfg, handle, NULL);
            em_tls_shutdown(t);
            em_tls_free(t);
        }
    }
    em_close(fd);
    return 0; /* a dropped connection must not cancel the others */
}

static int worker(void *arg)
{
    (void)arg;
    int lfd = em_tcp_listen((struct sockaddr *)&addr, sizeof addr, 4096);
    if (lfd < 0) return lfd;
    em_scope sc;
    em_scope_open(&sc, 0);
    int r = 0;
    while (!r) {
        int fd = em_accept(lfd);
        if (fd < 0) { r = fd; break; }
        if (em_spawn(&sc, conn, (void *)(intptr_t)fd) != 0) { em_close(fd); r = -ENOMEM; }
    }
    em_scope_cancel(&sc);
    em_close(lfd);
    em_scope_close(&sc);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4 || (argc == 4 && strcmp(argv[3], "tls"))) {
        fprintf(stderr, "usage: %s port [workers] [tls]\n", argv[0]);
        return 2;
    }
    int workers = argc >= 3 ? atoi(argv[2]) : 1;
    addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    memset(body4k, 'a', sizeof body4k);
    if (argc == 4) {
        SSL_CTX *cctx;
        tls_test_ctx(&sctx, &cctx);
        SSL_CTX_free(cctx);
    }
    int r = em_run(workers, worker, NULL);
    if (r) fprintf(stderr, "em_run: %d\n", r);
    return r != 0;
}
